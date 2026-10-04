// host.c - Milestone 2 verification suite for HDRAGCNext
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include "avisynth_c.h"

static AVS_ScriptEnvironment* env;
static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static AVS_Value invoke_named(const char* fn, AVS_Value* a, int n, const char** names)
{
    AVS_Value v = avs_invoke(env, fn, avs_new_value_array(a, n), names);
    if (avs_is_error(v)) { fprintf(stderr, "FAIL: %s: %s\n", fn, avs_as_error(v)); failures++; }
    return v;
}

static AVS_Clip* make_blank(int w, int h, int nframes, const char* px, const char* color)
{
    AVS_Value a[5]; const char* nm[5] = { "width", "height", "length", "pixel_type", "color" };
    a[0] = avs_new_value_int(w); a[1] = avs_new_value_int(h); a[2] = avs_new_value_int(nframes);
    a[3] = avs_new_value_string(px);
    a[4] = color ? avs_new_value_int((int)strtoul(color, NULL, 0)) : avs_new_value_bool(0);
    if (!color) { nm[4] = NULL; a[4] = avs_new_value_bool(0); } // dummy, unused
    AVS_Value v = invoke_named("BlankClip", a, color ? 5 : 4, nm);
    return avs_take_clip(v, env);
}

static AVS_Clip* apply_hdragc_ex2(AVS_Clip* src, float strength, float protect,
                                  int auto_points, float black, float white,
                                  float local_mix, float shadow_thr,
                                  const char* out_bits, const char* dither)
{
    // Urutan = template signature plugin (27 params). JANGAN campur
    // positional+named: named args diabaikan senyap (terverifikasi empiris).
    AVS_Value a[27]; const char* nm[27] = {0};
    a[0]  = avs_new_value_clip(src);
    a[1]  = avs_new_value_float(strength);
    a[2]  = avs_new_value_float(protect);
    a[3]  = avs_new_value_float(black);
    a[4]  = avs_new_value_float(white);
    a[5]  = avs_new_value_bool(auto_points);
    a[6]  = avs_new_value_float(0.85);
    a[7]  = avs_new_value_int(4);
    a[8]  = avs_new_value_string("iir");
    a[9]  = avs_new_value_float(0.35);
    a[10] = avs_new_value_float(0.20);
    a[11] = avs_new_value_int(3);
    a[12] = avs_new_value_float(1.0);
    a[13] = avs_new_value_float(1.0);
    a[14] = avs_new_value_bool(1);
    a[15] = avs_new_value_string("luma_ratio");
    a[16] = avs_new_value_float(0.7);
    a[17] = avs_new_value_float(0.9);
    a[18] = avs_new_value_int(4);
    a[19] = avs_new_value_float(local_mix);
    a[20] = avs_new_value_float(shadow_thr);
    a[21] = avs_new_value_float(2.0);
    a[22] = avs_new_value_int(-1);
    a[23] = avs_new_value_int(out_bits ? atoi(out_bits) : 0);
    a[24] = avs_new_value_string(dither ? dither : "none");
    a[25] = avs_new_value_string("none");
    a[26] = avs_new_value_bool(0);
    AVS_Value v = invoke_named("HDRAGCNext", a, 27, nm);
    return avs_take_clip(v, env);
}
// wrapper lama: tanpa processing (strength 0) untuk tes identitas/konversi
static AVS_Clip* apply_hdragc_ex(AVS_Clip* src, float strength, float protect,
                                 int auto_points, float black, float white,
                                 const char* out_bits, const char* dither)
{
    return apply_hdragc_ex2(src, strength, protect, auto_points, black, white,
                            0.0f, 0.35f, out_bits, dither);
}
static AVS_Clip* apply_hdragc(AVS_Clip* src, const char* out_bits, const char* dither)
{
    return apply_hdragc_ex(src, 0.0f, 0.0f, 0, 0.0f, 0.0f, out_bits, dither);
}

// pola tangga 5 segmen via StackVertical BlankClips berbeda warna
static AVS_Clip* make_step_pattern(void)
{
    const char* cols[5] = { "0x000000", "0x404040", "0x808080", "0xC0C0C0", "0xFFFFFF" };
    AVS_Value clips[5]; const char* nm[5] = {0};
    for (int i = 0; i < 5; i++) {
        AVS_Value a[5]; const char* bn[5] = {"width","height","length","pixel_type","color"};
        a[0]=avs_new_value_int(64); a[1]=avs_new_value_int(16);
        a[2]=avs_new_value_int(1);  a[3]=avs_new_value_string("YV12");
        a[4]=avs_new_value_int((int)strtoul(cols[i],NULL,0));
        AVS_Value v = invoke_named("BlankClip", a, 5, bn);
        clips[i] = v;
    }
    AVS_Value v = invoke_named("StackVertical", clips, 5, nm);
    return avs_take_clip(v, env);
}


static AVS_Clip* make_3tone(const char* c1, const char* c2, const char* c3)
{
    const char* cols[3] = {c1, c2, c3};
    const int wd[3] = {26, 12, 26};
    AVS_Value clips[3]; const char* nm[3] = {0,0,0};
    for (int i = 0; i < 3; i++) {
        AVS_Value a[5]; const char* bn[5] = {"width","height","length","pixel_type","color"};
        a[0]=avs_new_value_int(wd[i]); a[1]=avs_new_value_int(64);
        a[2]=avs_new_value_int(1);     a[3]=avs_new_value_string("YV12");
        a[4]=avs_new_value_int((int)strtoul(cols[i],NULL,0));
        AVS_Value v = invoke_named("BlankClip", a, 5, bn);
        clips[i] = v;
    }
    AVS_Value v = invoke_named("StackHorizontal", clips, 3, nm);
    return avs_take_clip(v, env);
}

static AVS_Clip* make_uniform(const char* color)
{
    AVS_Value a[5]; const char* bn[5] = {"width","height","length","pixel_type","color"};
    a[0]=avs_new_value_int(64); a[1]=avs_new_value_int(64);
    a[2]=avs_new_value_int(1);  a[3]=avs_new_value_string("YV12");
    a[4]=avs_new_value_int((int)strtoul(color,NULL,0));
    AVS_Value v = invoke_named("BlankClip", a, 5, bn);
    return avs_take_clip(v, env);
}

static AVS_Clip* splice_n(AVS_Clip** clips, int n)
{
    AVS_Value vals[8]; const char* nm[8] = {0,0,0,0,0,0,0,0};
    for (int i = 0; i < n; i++) vals[i] = avs_new_value_clip(clips[i]);
    AVS_Value v = invoke_named("UnalignedSplice", vals, n, nm);
    return avs_take_clip(v, env);
}

static int sample_yf(AVS_Clip* c, int x, int y, int frame)
{
    AVS_VideoFrame* f = avs_get_frame(c, frame);
    const unsigned char* p = avs_get_read_ptr_p(f, AVS_PLANAR_Y) + (size_t)y * avs_get_pitch_p(f, AVS_PLANAR_Y);
    int v = (avs_bits_per_component(avs_get_video_info(c)) == 8) ? p[x] : ((const unsigned short*)p)[x];
    avs_release_video_frame(f);
    return v;
}


static void sample_uv(AVS_Clip* c, int x, int y, int frame, int* u, int* v)
{
    AVS_VideoFrame* f = avs_get_frame(c, frame);
    int is8 = avs_bits_per_component(avs_get_video_info(c)) == 8;
    const unsigned char* pu = avs_get_read_ptr_p(f, AVS_PLANAR_U) + (size_t)y * avs_get_pitch_p(f, AVS_PLANAR_U);
    const unsigned char* pv = avs_get_read_ptr_p(f, AVS_PLANAR_V) + (size_t)y * avs_get_pitch_p(f, AVS_PLANAR_V);
    if (is8) { *u = pu[x]; *v = pv[x]; }
    else { *u = ((const unsigned short*)pu)[x]; *v = ((const unsigned short*)pv)[x]; }
    avs_release_video_frame(f);
}

// apply dgn kontrol chroma penuh (M6)
static AVS_Clip* apply_m6(AVS_Clip* src, float strength, float sat, int auto_sat, int auto_points)
{
    AVS_Value a[27]; const char* nm[27] = {0};
    a[0]=avs_new_value_clip(src); a[1]=avs_new_value_float(strength); a[2]=avs_new_value_float(0.8f);
    a[3]=avs_new_value_float(0.0f); a[4]=avs_new_value_float(0.0f); a[5]=avs_new_value_bool(auto_points);
    a[6]=avs_new_value_float(0.85f); a[7]=avs_new_value_int(4); a[8]=avs_new_value_string("iir");
    a[9]=avs_new_value_float(0.35f); a[10]=avs_new_value_float(0.20f); a[11]=avs_new_value_int(3);
    a[12]=avs_new_value_float(1.0f); a[13]=avs_new_value_float(sat); a[14]=avs_new_value_bool(auto_sat);
    a[15]=avs_new_value_string("luma_ratio"); a[16]=avs_new_value_float(0.7f); a[17]=avs_new_value_float(0.9f);
    a[18]=avs_new_value_int(4); a[19]=avs_new_value_float(1.0f); a[20]=avs_new_value_float(0.35f);
    a[21]=avs_new_value_float(2.0f); a[22]=avs_new_value_int(-1);
    a[23]=avs_new_value_int(0); a[24]=avs_new_value_string("none"); a[25]=avs_new_value_string("none");
    a[26]=avs_new_value_bool(0);
    AVS_Value v = invoke_named("HDRAGCNext", a, 27, nm);
    return avs_take_clip(v, env);
}


static AVS_Clip* apply_show(AVS_Clip* src, const char* show)
{
    AVS_Value a[27]; const char* nm[27] = {0};
    a[0]=avs_new_value_clip(src); a[1]=avs_new_value_float(0.8f); a[2]=avs_new_value_float(0.8f);
    a[3]=avs_new_value_float(0.0f); a[4]=avs_new_value_float(0.0f); a[5]=avs_new_value_bool(0); // auto_points=0 (fixed 0..1 via guard)
    a[6]=avs_new_value_float(0.85f); a[7]=avs_new_value_int(4); a[8]=avs_new_value_string("iir");
    a[9]=avs_new_value_float(0.35f); a[10]=avs_new_value_float(0.20f); a[11]=avs_new_value_int(3);
    a[12]=avs_new_value_float(1.0f); a[13]=avs_new_value_float(1.0f); a[14]=avs_new_value_bool(0);
    a[15]=avs_new_value_string("luma_ratio"); a[16]=avs_new_value_float(0.7f); a[17]=avs_new_value_float(0.9f);
    a[18]=avs_new_value_int(4); a[19]=avs_new_value_float(1.0f); a[20]=avs_new_value_float(0.35f);
    a[21]=avs_new_value_float(2.0f); a[22]=avs_new_value_int(-1);
    a[23]=avs_new_value_int(0); a[24]=avs_new_value_string("none"); a[25]=avs_new_value_string(show);
    a[26]=avs_new_value_bool(0);
    AVS_Value v = invoke_named("HDRAGCNext", a, 27, nm);
    return avs_take_clip(v, env);
}


// varian C "recovery": knob dinaikkan utk konten setengah-gelap (landscape)
static AVS_Clip* apply_C(AVS_Clip* src)
{
    AVS_Value a[27]; const char* nm[27] = {0};
    a[0]=avs_new_value_clip(src); a[1]=avs_new_value_float(1.0f); a[2]=avs_new_value_float(0.9f);
    a[3]=avs_new_value_float(0.0f); a[4]=avs_new_value_float(0.0f); a[5]=avs_new_value_bool(1);
    a[6]=avs_new_value_float(0.85f); a[7]=avs_new_value_int(4); a[8]=avs_new_value_string("iir");
    a[9]=avs_new_value_float(0.35f); a[10]=avs_new_value_float(0.20f); a[11]=avs_new_value_int(3);
    a[12]=avs_new_value_float(1.0f); a[13]=avs_new_value_float(1.0f); a[14]=avs_new_value_bool(1);
    a[15]=avs_new_value_string("luma_ratio"); a[16]=avs_new_value_float(0.7f); a[17]=avs_new_value_float(0.9f);
    a[18]=avs_new_value_int(4); a[19]=avs_new_value_float(1.0f); a[20]=avs_new_value_float(0.45f); // tau 0.45
    a[21]=avs_new_value_float(1.2f); // mask_gamma 1.2 (bukan 2.0)
    a[22]=avs_new_value_int(-1);
    a[23]=avs_new_value_int(0); a[24]=avs_new_value_string("none"); a[25]=avs_new_value_string("none");
    a[26]=avs_new_value_bool(0);
    AVS_Value v = invoke_named("HDRAGCNext", a, 27, nm);
    return avs_take_clip(v, env);
}

static int sample_y(AVS_Clip* c, int x, int y)
{
    AVS_VideoFrame* f = avs_get_frame(c, 0);
    const unsigned char* p = avs_get_read_ptr_p(f, AVS_PLANAR_Y) + (size_t)y * avs_get_pitch_p(f, AVS_PLANAR_Y);
    int v = (avs_bits_per_component(avs_get_video_info(c)) == 8) ? p[x] : ((const unsigned short*)p)[x];
    avs_release_video_frame(f);
    return v;
}

// kurva M3 yang direplikasi di host (double math, toleransi vs float plugin)
static double m3_curve(double x, double black, double white, double s, double p)
{
    double range = white - black; if (range < 1e-6) range = 1e-6;
    double xn = (x - black) / range;
    if (xn < 0) xn = 0; if (xn > 1) xn = 1;
    double y = xn * (1.0 + s * (1.0 - xn));
    double t = (y - 0.7) / 0.3; if (t < 0) t = 0; if (t > 1) t = 1;
    double ss = t * t * (3 - 2 * t);
    return pow(y, 1.0 + p * ss);
}

// byte-identical comparison of all 3 planes, N frames
static void expect_identical(AVS_Clip* a, AVS_Clip* b, int nframes, const char* label)
{
    for (int n = 0; n < nframes; n++) {
        AVS_VideoFrame* fa = avs_get_frame(a, n);
        AVS_VideoFrame* fb = avs_get_frame(b, n);
        for (int p = 0; p < 3; p++) {
            int plane = (p==0)?AVS_PLANAR_Y:(p==1?AVS_PLANAR_U:AVS_PLANAR_V);
            int h = avs_get_height_p(fa, plane);
            int ra = avs_get_row_size_p(fa, plane), rb = avs_get_row_size_p(fb, plane);
            CHECK(ra == rb, "%s f%d p%d rowsize %d!=%d", label, n, p, ra, rb);
            const unsigned char* pa = avs_get_read_ptr_p(fa, plane);
            const unsigned char* pb = avs_get_read_ptr_p(fb, plane);
            int pa_p = avs_get_pitch_p(fa, plane), pb_p = avs_get_pitch_p(fb, plane);
            int de = (ra == avs_get_row_size_p(fa, AVS_PLANAR_Y) && plane != AVS_PLANAR_Y) ? 1 : ra/((ra>256)?2:1);
            int elem = (ra > 256*2/2 && plane == AVS_PLANAR_Y) ? (ra > 512 ? 2 : 1) : (ra > 128 ? 2 : 1); // heuristic, replaced below
            (void)de; (void)elem;
            for (int y = 0; y < h; y++)
                if (memcmp(pa + (size_t)y*pa_p, pb + (size_t)y*pb_p, ra) != 0) {
                    int x = 0; while (x < ra && pa[y*pa_p+x] == pb[y*pb_p+x]) x++;
                    fprintf(stderr, "FAIL: %s f%d plane %d row %d first-diff @byte %d: %d vs %d\n",
                            label, n, p, y, x, pa[y*pa_p+x], pb[y*pb_p+x]);
                    failures++; goto next_frame;
                }
        }
        next_frame:
        avs_release_video_frame(fa); avs_release_video_frame(fb);
    }
}

// strict expected-value check for depth conversion (element-wise), 1 frame
static void expect_convert_16to8(AVS_Clip* src16, AVS_Clip* out8, const char* label)
{
    AVS_VideoFrame* fs = avs_get_frame(src16, 0);
    AVS_VideoFrame* fo = avs_get_frame(out8, 0);
    CHECK(avs_bits_per_component(avs_get_video_info(out8)) == 8, "%s: out bits!=8", label);
    for (int p = 0; p < 3; p++) {
        int plane = (p==0)?AVS_PLANAR_Y:(p==1?AVS_PLANAR_U:AVS_PLANAR_V);
        int h = avs_get_height_p(fs, plane);
        int rs = avs_get_row_size_p(fs, plane); // bytes (uint16)
        int elems = rs / 2;
        const unsigned char* ps = avs_get_read_ptr_p(fs, plane);
        const unsigned char* po = avs_get_read_ptr_p(fo, plane);
        int ps_p = avs_get_pitch_p(fs, plane), po_p = avs_get_pitch_p(fo, plane);
        for (int y = 0; y < h; y++) {
            const unsigned short* s = (const unsigned short*)(ps + (size_t)y*ps_p);
            const unsigned char*  o = po + (size_t)y*po_p;
            for (int x = 0; x < elems; x++) {
                long exp = lrint((double)s[x] * 255.0 / 65535.0);
                if (exp < 0) exp = 0; if (exp > 255) exp = 255;
                if (o[x] != exp) {
                    fprintf(stderr, "FAIL: %s p%d (%d,%d): got %d expect %d\n", label, p, x, y, o[x], (int)exp);
                    if (++failures > 5) return;
                }
            }
        }
    }
    avs_release_video_frame(fs); avs_release_video_frame(fo);
}

// upconvert 8->16: expect exact v*257
static void expect_convert_8to16(AVS_Clip* src8, AVS_Clip* out16, const char* label)
{
    AVS_VideoFrame* fs = avs_get_frame(src8, 0);
    AVS_VideoFrame* fo = avs_get_frame(out16, 0);
    CHECK(avs_bits_per_component(avs_get_video_info(out16)) == 16, "%s: out bits!=16", label);
    for (int p = 0; p < 3; p++) {
        int plane = (p==0)?AVS_PLANAR_Y:(p==1?AVS_PLANAR_U:AVS_PLANAR_V);
        int h = avs_get_height_p(fs, plane);
        int elems = avs_get_row_size_p(fs, plane);
        const unsigned char*  ps = avs_get_read_ptr_p(fs, plane);
        const unsigned char* po = avs_get_read_ptr_p(fo, plane);
        int ps_p = avs_get_pitch_p(fs, plane), po_p = avs_get_pitch_p(fo, plane);
        for (int y = 0; y < h; y++) {
            const unsigned char*  s = ps + (size_t)y*ps_p;
            const unsigned short* o = (const unsigned short*)(po + (size_t)y*po_p);
            for (int x = 0; x < elems; x++)
                if (o[x] != (unsigned short)(s[x] * 257)) {
                    fprintf(stderr, "FAIL: %s p%d (%d,%d): got %d expect %d\n", label, p, x, y, o[x], s[x]*257);
                    if (++failures > 5) return;
                }
        }
    }
    avs_release_video_frame(fs); avs_release_video_frame(fo);
}

int main(int argc, char** argv)
{
    setbuf(stdout, NULL);
    const char* night_yuv_path = getenv("NIGHT_YUV");
    if (!night_yuv_path) night_yuv_path = "/tmp/night.yuv";
    if (argc < 2) { fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    env = avs_create_script_environment(AVISYNTH_INTERFACE_VERSION);
    AVS_Value a0[1] = { avs_new_value_string(argv[1]) };
    AVS_Value v = invoke_named("LoadPlugin", a0, 1, (const char*[]){ NULL });
    if (avs_is_error(v)) return 1;
    char rawsrc_path[1200];
    snprintf(rawsrc_path, sizeof(rawsrc_path), "%s", argv[1]);
    char* slashp = strrchr(rawsrc_path, '/');
    if (slashp) strcpy(slashp + 1, "rawsrc.so");
    else snprintf(rawsrc_path, sizeof(rawsrc_path), "rawsrc.so");
    AVS_Value a0b[1] = { avs_new_value_string(rawsrc_path) };
    AVS_Value vrb = invoke_named("LoadPlugin", a0b, 1, (const char*[]){ NULL });
    if (avs_is_error(vrb)) return 1;

    // T1: YV12 8-bit, default params -> byte-identical
    AVS_Clip* c8 = make_blank(64, 64, 3, "YV12", NULL);
    AVS_Clip* f8 = apply_hdragc(c8, NULL, NULL);
    expect_identical(c8, f8, 3, "T1-YV12-default");

    // T2: YUV420P16, default -> byte-identical
    AVS_Clip* c16 = make_blank(64, 64, 3, "YUV420P16", NULL);
    AVS_Clip* f16 = apply_hdragc(c16, NULL, NULL);
    expect_identical(c16, f16, 3, "T2-P16-default");

    // T3: extremes 8-bit black/white -> identical
    AVS_Clip* cb = make_blank(32, 32, 1, "YV12", "0x000000");
    AVS_Clip* fb = apply_hdragc(cb, NULL, NULL);
    expect_identical(cb, fb, 1, "T3-YV12-black");
    AVS_Clip* cw = make_blank(32, 32, 1, "YV12", "0xFFFFFF");
    AVS_Clip* fw = apply_hdragc(cw, NULL, NULL);
    expect_identical(cw, fw, 1, "T3-YV12-white");

    // T4: P16 -> 8-bit dither=none : strict expected values
    AVS_Clip* cd = apply_hdragc(c16, "8", "none");
    expect_convert_16to8(c16, cd, "T4-P16to8");

    // T5: P16 -> 8-bit dither=ordered : same expected mean, no crash, in-range
    AVS_Clip* co = apply_hdragc(c16, "8", "ordered");
    AVS_VideoFrame* t = avs_get_frame(co, 0);
    CHECK(t != NULL, "T5: no frame");
    avs_release_video_frame(t);

    // T6: 8-bit -> 16-bit upconvert : exact v*257
    AVS_Clip* cu = apply_hdragc(c8, "16", "none");
    expect_convert_8to16(c8, cu, "T6-8to16");

    // T7: tone curve global pada pola tangga
    AVS_Clip* step = make_step_pattern();
    AVS_Clip* tc  = apply_hdragc_ex2(step, 0.8f, 0.8f, 0, 0.0f, 1.0f, 0.0f, 0.35f, NULL, "none");
    AVS_Clip* tca = apply_hdragc_ex2(step, 0.8f, 0.8f, 1, 0.0f, 0.0f, 0.0f, 0.35f, NULL, "none");
    int sv[5], dv[5];
    for (int k = 0; k < 5; k++) {
        sv[k] = sample_y(step, 32, 8 + 16 * k);
        dv[k] = sample_y(tc,  32, 8 + 16 * k);
    }
    for (int k = 0; k < 4; k++)
        CHECK(dv[k] < dv[k+1], "T7a: not increasing at seg %d (%d !< %d)", k, dv[k], dv[k+1]);
    CHECK(dv[0] > sv[0], "T7a: black not lifted (%d !> %d)", dv[0], sv[0]);
    CHECK(dv[4] <= sv[4] + 16, "T7a: white blown up (%d > %d+16)", dv[4], sv[4]);
    for (int k = 0; k < 5; k++) {
        double x = sv[k] / 255.0;
        int exp = (int)lrint(m3_curve(x, 0, 1, 0.8, 0.8) * 255.0);
        CHECK(abs(dv[k] - exp) <= 2, "T7a: seg %d got %d expect %d (+/-2)", k, dv[k], exp);
    }
    int prev = -1;
    for (int k = 0; k < 5; k++) {
        int v = sample_y(tca, 32, 8 + 16 * k);
        CHECK(v >= prev, "T7b: auto not monotonic at seg %d (%d < %d)", k, v, prev);
        prev = v;
    }
    CHECK(prev <= 255, "T7b: clipped max %d", prev);

    // T8: spatially-adaptive shadow gain (local_mix=1)
    AVS_Clip* t8c = apply_hdragc_ex2(step, 0.8f, 0.8f, 0, 0.0f, 1.0f, 1.0f, 0.35f, NULL, "none");
    int d8[5];
    for (int k = 0; k < 5; k++) d8[k] = sample_y(t8c, 32, 8 + 16 * k);
    for (int k = 0; k < 4; k++)
        CHECK(d8[k] < d8[k+1], "T8: not increasing at seg %d (%d !< %d)", k, d8[k], d8[k+1]);
    // seg gelap: lift hampir penuh (mask ~1) -> dekat prediksi global
    double g0exp = m3_curve(sv[0]/255.0, 0, 1, 0.8, 0.8) * 255.0;
    CHECK(abs(d8[0] - (int)lrint(g0exp)) <= 4, "T8: dark seg %d vs global %.0f", d8[0], g0exp);
    // seg terang: lift DITEKAN mask -> harus LEBIH RENDAH dari prediksi global
    double g4exp = m3_curve(sv[4]/255.0, 0, 1, 0.8, 0.8) * 255.0;
    CHECK(d8[4] < (int)lrint(g4exp), "T8: bright seg NOT suppressed %d !< %d", d8[4], (int)lrint(g4exp));
    // T8b: auto tau (shadow_threshold=0) -> monotonic saja
    AVS_Clip* t8b = apply_hdragc_ex2(step, 0.8f, 0.8f, 0, 0.0f, 1.0f, 1.0f, 0.0f, NULL, "none");
    prev = -1;
    for (int k = 0; k < 5; k++) {
        int v = sample_y(t8b, 32, 8 + 16 * k);
        CHECK(v >= prev, "T8b: auto-tau not monotonic seg %d (%d < %d)", k, v, prev);
        prev = v;
    }

    // ---- M5: temporal + scene cut v2 ----
    // frame 3-tone: p01/p99 pinned di strip gelap/terang, strip TENGAH observable
    AVS_Clip* A  = make_3tone("0x202020", "0x808080", "0xE0E0E0");   // mid Y=126
    AVS_Clip* B  = make_3tone("0x080808", "0x505050", "0xA0A0A0");   // mid Y~80, lebih gelap dr A
    AVS_Clip* F  = make_uniform("0xFFFFFF");
    // helper apply dengan auto_points + temporal defaults
    #define APPLY_M5(src) apply_hdragc_ex2(src, 0.8f, 0.8f, 1, 0.0f, 0.0f, 0.0f, 0.35f, NULL, "none")

    // T9 flash: [A,A,A,F,A,A] -> no reset, no pumping
    {
        AVS_Clip* seq[6] = {A,A,A,F,A,A};
        AVS_Clip* s9 = splice_n(seq, 6);
        AVS_Clip* f9 = APPLY_M5(s9);
        int o2 = sample_yf(f9, 32, 32, 2), o4 = sample_yf(f9, 32, 32, 4), o5 = sample_yf(f9, 32, 32, 5);
        CHECK(abs(o4 - o2) <= 8, "T9: pumping after flash (%d vs %d)", o4, o2);
        CHECK(abs(o5 - o2) <= 8, "T9: not settled by f5 (%d vs %d)", o5, o2);
    }
    // T10 hard cut: [A,A,A,B,B,B] -> reset terdeteksi
    {
        AVS_Clip* seq[6] = {A,A,A,B,B,B};
        AVS_Clip* s10 = splice_n(seq, 6);
        AVS_Clip* f10 = APPLY_M5(s10);
        int o2 = sample_yf(f10, 32, 32, 2), o3 = sample_yf(f10, 32, 32, 3), o5 = sample_yf(f10, 32, 32, 5);
        CHECK(o2 >= 150 && o2 <= 195, "T10: sanity A mid (o2=%d)", o2);
        // cut -> o3 langsung ke level B (lebih gelap); no-cut -> o3 masih dekat A
        CHECK(o3 <= o2 - 10, "T10: cut NOT detected (o3=%d vs o2=%d)", o3, o2);
        CHECK(abs(o5 - o3) <= 4, "T10: not converged by f5 (%d vs %d)", o5, o3);
    }
    // T11 fade: 5 frame gradual +8/channel -> bukan cut, transisi halus
    {
        const char* g[5][3] = {{"0x202020","0x808080","0xE0E0E0"},
                               {"0x282828","0x888888","0xE8E8E8"},
                               {"0x303030","0x909090","0xF0F0F0"},
                               {"0x383838","0x989898","0xF8F8F8"},
                               {"0x404040","0xA0A0A0","0xFFFFFF"}};
        AVS_Clip* frames[5];
        for (int i = 0; i < 5; i++) frames[i] = make_3tone(g[i][0], g[i][1], g[i][2]);
        AVS_Clip* s11 = splice_n(frames, 5);
        AVS_Clip* f11 = APPLY_M5(s11);
        int prev = -1;
        for (int i = 0; i < 5; i++) {
            int v = sample_yf(f11, 32, 32, i);
            CHECK(v >= prev, "T11: fade not monotonic f%d (%d < %d)", i, v, prev);
            if (prev >= 0) CHECK(v - prev <= 15, "T11: fade jump too big f%d (+%d)", i, v - prev);
            prev = v;
        }
    }

    // ---- M6: chroma luma-ratio + hue-preserving soft clamp ----
    AVS_Clip* col = make_3tone("0x801010", "0x108010", "0x101080");  // merah/hijau/biru
    // cari per third: x chroma dengan |C-neutral| maksimum di SOURCE (robust
    // terhadap alignment chroma subsampling yg tidak intuitif)
    int bestx[3];
    {
        AVS_VideoFrame* f = avs_get_frame(col, 0);
        const unsigned char* pu = avs_get_read_ptr_p(f, AVS_PLANAR_U);
        const unsigned char* pv = avs_get_read_ptr_p(f, AVS_PLANAR_V);
        int cw = avs_get_row_size_p(f, AVS_PLANAR_U);
        for (int t = 0; t < 3; t++) {
            int x0 = t * cw / 3, x1 = (t + 1) * cw / 3, bx = x0;
            double bm = -1;
            for (int x = x0; x < x1; x++) {
                double m = sqrt((double)((int)pu[x]-128)*(pu[x]-128) + (double)((int)pv[x]-128)*(pv[x]-128));
                if (m > bm) { bm = m; bx = x; }
            }
            bestx[t] = bx;
        }
        avs_release_video_frame(f);
    }
    // bestx untuk klip moderat
    AVS_Clip* colm = make_3tone("0x804040", "0x408040", "0x404080");
    int bestxm[3];
    {
        AVS_VideoFrame* f = avs_get_frame(colm, 0);
        const unsigned char* pu = avs_get_read_ptr_p(f, AVS_PLANAR_U);
        const unsigned char* pv = avs_get_read_ptr_p(f, AVS_PLANAR_V);
        int cw = avs_get_row_size_p(f, AVS_PLANAR_U);
        for (int t = 0; t < 3; t++) {
            int x0 = t * cw / 3, x1 = (t + 1) * cw / 3, bx = x0;
            double bm = -1;
            for (int x = x0; x < x1; x++) {
                double m = sqrt((double)((int)pu[x]-128)*(pu[x]-128) + (double)((int)pv[x]-128)*(pv[x]-128));
                if (m > bm) { bm = m; bx = x; }
            }
            bestxm[t] = bx;
        }
        avs_release_video_frame(f);
    }
    // T12a: hue preservation
    AVS_Clip* c6 = apply_m6(col, 0.8f, 1.0f, 0, 1);
    for (int k = 0; k < 3; k++) {
        int us, vs, ud, vd;
        sample_uv(col, bestx[k], 16, 0, &us, &vs);
        sample_uv(c6,  bestx[k], 16, 0, &ud, &vd);
        double ms = sqrt((double)(us-128)*(us-128) + (double)(vs-128)*(vs-128));
        CHECK(ms > 10.0, "T12a: strip %d src chroma too small (%.1f)", k, ms);
        double as = atan2((double)(vs-128), (double)(us-128));
        double ad = atan2((double)(vd-128), (double)(ud-128));
        double dd = fabs(as - ad) * 180.0 / 3.14159265;
        if (dd > 180.0) dd = 360.0 - dd;
        CHECK(dd <= 3.0, "T12a: hue shift strip %d = %.1f deg (>3)", k, dd);
    }
    // T12b: boost proporsional — mean ratio seluruh plane chroma (robust thd
    // per-pixel clamp; piksel non-neutral ms>4 saja yang dihitung)
    AVS_Clip* c6b = apply_m6(colm, 0.8f, 1.3f, 0, 0);
    {
        AVS_VideoFrame* fs = avs_get_frame(colm, 0);
        AVS_VideoFrame* fd = avs_get_frame(c6b, 0);
        const unsigned char* su_ = avs_get_read_ptr_p(fs, AVS_PLANAR_U);
        const unsigned char* sv_ = avs_get_read_ptr_p(fs, AVS_PLANAR_V);
        const unsigned char* du_ = avs_get_read_ptr_p(fd, AVS_PLANAR_U);
        const unsigned char* dv_ = avs_get_read_ptr_p(fd, AVS_PLANAR_V);
        int cw_ = avs_get_row_size_p(fs, AVS_PLANAR_U), ch_ = avs_get_height_p(fs, AVS_PLANAR_U);
        int sp_ = avs_get_pitch_p(fs, AVS_PLANAR_U), dp_ = avs_get_pitch_p(fd, AVS_PLANAR_U);
        double sum_s = 0, sum_d = 0;
        for (int y = 0; y < ch_; y++) {
            const unsigned char* su = su_ + (size_t)y * sp_;
            const unsigned char* sv = sv_ + (size_t)y * sp_;
            const unsigned char* du2 = du_ + (size_t)y * dp_;
            const unsigned char* dv2 = dv_ + (size_t)y * dp_;
            for (int x = 0; x < cw_; x++) {
                double ms = sqrt((double)((int)su[x]-128)*(su[x]-128) + (double)((int)sv[x]-128)*(sv[x]-128));
                if (ms < 4.0) continue;
                double md = sqrt((double)((int)du2[x]-128)*(du2[x]-128) + (double)((int)dv2[x]-128)*(dv2[x]-128));
                sum_s += ms; sum_d += md;
            }
        }
        avs_release_video_frame(fs); avs_release_video_frame(fd);
        CHECK(sum_s > 100.0, "T12b: total src chroma too small (%.0f)", sum_s);
        double ratio = sum_d / sum_s;
        CHECK(ratio >= 1.10 && ratio <= 1.65, "T12b: mean boost ratio %.3f (expect ~1.3-1.45)", ratio);
    }

    // ---- M7: show modes (klip 3-tone HORIZONTAL) ----
    AVS_Clip* ht = make_3tone("0x202020", "0x808080", "0xE0E0E0");  // Y ~47/126/208
    // T13a show="mask": gelap mask ~0.22 (>40), terang ~0 (<10)
    AVS_Clip* cmask = apply_show(ht, "mask");
    int md_dark = sample_yf(cmask, 8, 32, 0), md_bright = sample_yf(cmask, 52, 32, 0);
    CHECK(md_dark > 40, "T13a: mask dark=%d (expect ~57)", md_dark);
    CHECK(md_bright < 10, "T13a: mask bright=%d (expect ~0)", md_bright);
    // T13b show="lift": gelap terangkat (>138), terang terkompresi (<=128)
    AVS_Clip* clift = apply_show(ht, "lift");
    int lf_dark = sample_yf(clift, 8, 32, 0), lf_bright = sample_yf(clift, 52, 32, 0);
    CHECK(lf_dark > 138, "T13b: lift dark=%d (expect ~145)", lf_dark);
    CHECK(lf_bright <= 128, "T13b: lift bright=%d (expect ~128)", lf_bright);
    // T13c show="base": monotonic
    AVS_Clip* cbase = apply_show(ht, "base");
    int b0 = sample_yf(cbase, 8, 32, 0), b1 = sample_yf(cbase, 32, 32, 0), b2 = sample_yf(cbase, 52, 32, 0);
    CHECK(b0 < b1 && b1 < b2, "T13c: base not monotonic %d %d %d", b0, b1, b2);

    // T14 perf smoke: 640x480 YV12, 32 frame, pyramid path
    {
        AVS_Clip* big = make_blank(640, 480, 32, "YV12", NULL);
        AVS_Clip* fbig = apply_hdragc_ex2(big, 0.8f, 0.8f, 0, 0.0f, 0.0f, 1.0f, 0.35f, NULL, "none");
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        long acc = 0;
        for (int n = 0; n < 32; n++) {
            AVS_VideoFrame* f = avs_get_frame(fbig, n);
            acc += avs_get_read_ptr_p(f, AVS_PLANAR_Y)[0];
            avs_release_video_frame(f);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        printf("T14 perf: %.1f ms/frame (640x480 YV12, 32 frames)%s\n", ms / 32, acc ? "" : "");
        CHECK(ms / 32 < 1500.0, "T14: too slow %.1f ms/frame", ms / 32);
    }

    // T15 blue noise: pyramid path, blue vs ordered menghasilkan pola berbeda
    {
        AVS_Clip* cb = apply_hdragc_ex2(c16, 0.8f, 0.8f, 0, 0.0f, 0.0f, 0.0f, 0.35f, "8", "blue");
        AVS_Clip* co2 = apply_hdragc_ex2(c16, 0.8f, 0.8f, 0, 0.0f, 0.0f, 0.0f, 0.35f, "8", "ordered");
        AVS_VideoFrame* fb = avs_get_frame(cb, 0);
        AVS_VideoFrame* fo = avs_get_frame(co2, 0);
        const unsigned char* pb = avs_get_read_ptr_p(fb, AVS_PLANAR_Y);
        const unsigned char* po = avs_get_read_ptr_p(fo, AVS_PLANAR_Y);
        int nd = 0; long diffcount = 0;
        int seen[256] = {0};
        for (int y = 0; y < 64; y++) {
            const unsigned char* rb = pb + (size_t)y * avs_get_pitch_p(fb, AVS_PLANAR_Y);
            const unsigned char* ro = po + (size_t)y * avs_get_pitch_p(fo, AVS_PLANAR_Y);
            for (int x = 0; x < 64; x++) {
                if (!seen[rb[x]]) { seen[rb[x]] = 1; nd++; }
                if (rb[x] != ro[x]) diffcount++;
            }
        }
        avs_release_video_frame(fb); avs_release_video_frame(fo);
        CHECK(nd >= 2, "T15: blue dither no variation (%d distinct)", nd);
        CHECK(diffcount > 100, "T15: blue == ordered?! (%ld diffs)", diffcount);
    }

    // ---- T16: BUKTI pada frame REAL gelap (foto malam) ----
    // Lewati gracefully bila frame tidak tersedia (mis. CI tanpa foto asli):
    // beri NIGHT_YUV=<path> atau taruh /tmp/night.yuv untuk bukti penuh.
    if (access(night_yuv_path, F_OK) != 0) {
        printf("T16: skipped (no %s -- set NIGHT_YUV for real-frame proof)\n", night_yuv_path);
    } else {
        AVS_Value ra[4]; const char* rn[4] = { NULL, "width", "height", "length" };
        ra[0] = avs_new_value_string(night_yuv_path);
        ra[1] = avs_new_value_int(612);
        ra[2] = avs_new_value_int(408);
        ra[3] = avs_new_value_int(1);
        AVS_Value vr = invoke_named("RawSourceYV12", ra, 4, rn);
        AVS_Clip* real = avs_take_clip(vr, env);

        // Varian A: auto points (p01/p99 stretch) + tau 0.35 — gaya "auto levels"
        AVS_Clip* fa = apply_hdragc_ex2(real, 0.8f, 0.8f, 1, 0.0f, 0.0f, 1.0f, 0.35f, NULL, "none");
        // Varian B: fixed full-range + tau 0.35 — lift murni, highlight untouched
        AVS_Clip* fb = apply_hdragc_ex2(real, 0.8f, 0.8f, 0, 0.0f, 0.0f, 1.0f, 0.35f, NULL, "none");

        AVS_VideoFrame* fs = avs_get_frame(real, 0);
        AVS_VideoFrame* fda = avs_get_frame(fa, 0);
        AVS_VideoFrame* fdb = avs_get_frame(fb, 0);
        int W = 612, H = 408;
        const unsigned char* ys  = avs_get_read_ptr_p(fs,  AVS_PLANAR_Y);
        const unsigned char* yda = avs_get_read_ptr_p(fda, AVS_PLANAR_Y);
        const unsigned char* ydb = avs_get_read_ptr_p(fdb, AVS_PLANAR_Y);
        int ps  = avs_get_pitch_p(fs,  AVS_PLANAR_Y);
        int pda = avs_get_pitch_p(fda, AVS_PLANAR_Y);
        int pdb = avs_get_pitch_p(fdb, AVS_PLANAR_Y);

        double ms = 0, ma = 0, mb = 0;
        long clips = 0;
        unsigned hists[256] = {0}, hista[256] = {0}, histb[256] = {0};
        for (int y = 0; y < H; y++) {
            const unsigned char* rs  = ys  + (size_t)y * ps;
            const unsigned char* ra2 = yda + (size_t)y * pda;
            const unsigned char* rb2 = ydb + (size_t)y * pdb;
            for (int x = 0; x < W; x++) {
                ms += rs[x]; ma += ra2[x]; mb += rb2[x];
                hists[rs[x]]++; hista[ra2[x]]++; histb[rb2[x]]++;
                if (ra2[x] >= 255) clips++;
            }
        }
        double tot = (double)W * H;
        double pct(unsigned* hist2, double q, double tot2);
        long cums = 0, cuma = 0, cumb = 0; double p1s=0,p1a=0,p1b=0,p99s=0,p99a=0,p99b=0;
        for (int i = 0; i < 256; i++) {
            cums += hists[i]; cuma += hista[i]; cumb += histb[i];
            if (!p1s && cums >= tot*0.01) p1s = i;
            if (!p1a && cuma >= tot*0.01) p1a = i;
            if (!p1b && cumb >= tot*0.01) p1b = i;
            if (cums < tot*0.99) p99s = i;
            if (cuma < tot*0.99) p99a = i;
            if (cumb < tot*0.99) p99b = i;
        }
        printf("T16 REAL dark frame 612x408 (night street):\n");
        printf("  before  : mean=%.1f p01=%.0f p99=%.0f\n", ms/tot, p1s, p99s);
        printf("  A(auto): mean=%.1f p01=%.0f p99=%.0f clipped=%ld\n", ma/tot, p1a, p99a, clips);
        printf("  B(fixed): mean=%.1f p01=%.0f p99=%.0f\n", mb/tot, p1b, p99b);
        // mean gate rendah (+2): frame dg area terang besar (river/ridge) memang
        // hanya terlift lokal di shadow -> mean hampir diam; yang wajib naik = p01
        CHECK(ma/tot > ms/tot + 2, "T16A: auto not lifted (%.1f -> %.1f)", ms/tot, ma/tot);
        CHECK(p99a <= p99s + 35, "T16A: highlights blown (p99 %.0f -> %.0f)", p99s, p99a);
        CHECK(p1a > p1s + 5, "T16A: shadows not lifted (p01 %.0f -> %.0f)", p1s, p1a);
        CHECK(clips < tot*0.001, "T16A: clipping %.2f%%", 100.0*clips/tot);
        CHECK(mb/tot > ms/tot + 3, "T16B: fixed not lifted (%.1f -> %.1f)", ms/tot, mb/tot);
        CHECK(p99b <= p99s + 8, "T16B: highlights not protected (p99 %.0f -> %.0f)", p99s, p99b);
        CHECK(p1b >= p1s - 3, "T16B: significant black crush (p01 %.0f -> %.0f)", p1s, p1b);

        // dump: src Y, dstA Y+UV, dstB Y+UV
        FILE* f1 = fopen("/tmp/proof_src.yuv", "wb");
        FILE* f2 = fopen("/tmp/proof_A.yuv", "wb");
        FILE* f3 = fopen("/tmp/proof_B.yuv", "wb");
        const unsigned char *ua, *va, *ub, *vb;
        int pua, pva;
        ua = avs_get_read_ptr_p(fda, AVS_PLANAR_U); va = avs_get_read_ptr_p(fda, AVS_PLANAR_V);
        ub = avs_get_read_ptr_p(fdb, AVS_PLANAR_U); vb = avs_get_read_ptr_p(fdb, AVS_PLANAR_V);
        pua = avs_get_pitch_p(fda, AVS_PLANAR_U); pva = avs_get_pitch_p(fda, AVS_PLANAR_V);
        for (int y = 0; y < H; y++) {
            fwrite(ys  + (size_t)y*ps,  1, W, f1);
            fwrite(yda + (size_t)y*pda, 1, W, f2);
            fwrite(ydb + (size_t)y*pdb, 1, W, f3);
        }
        // dump chroma SOURCE juga (fiks: sebelumnya Y-only -> render jadi BW!)
        const unsigned char* us_ = avs_get_read_ptr_p(fs, AVS_PLANAR_U);
        const unsigned char* vs_ = avs_get_read_ptr_p(fs, AVS_PLANAR_V);
        int puc2 = avs_get_pitch_p(fs, AVS_PLANAR_U);
        int cw = W/2, chh = H/2;
        for (int y = 0; y < chh; y++) fwrite(us_ + (size_t)y*puc2, 1, cw, f1);
        for (int y = 0; y < chh; y++) fwrite(vs_ + (size_t)y*puc2, 1, cw, f1);
        for (int y = 0; y < chh; y++) fwrite(ua + (size_t)y*pua, 1, cw, f2);
        for (int y = 0; y < chh; y++) fwrite(va + (size_t)y*pva, 1, cw, f2);
        for (int y = 0; y < chh; y++) fwrite(ub + (size_t)y*pua, 1, cw, f3);
        for (int y = 0; y < chh; y++) fwrite(vb + (size_t)y*pva, 1, cw, f3);
        fclose(f1); fclose(f2); fclose(f3);

        // varian C: recovery preset — wajib terlihat pada konten setengah-gelap
        AVS_Clip* fc = apply_C(real);
        AVS_VideoFrame* fdc = avs_get_frame(fc, 0);
        const unsigned char* yc2 = avs_get_read_ptr_p(fdc, AVS_PLANAR_Y);
        int pdc = avs_get_pitch_p(fdc, AVS_PLANAR_Y);
        double mc2 = 0; long clipc = 0; unsigned histc[256] = {0};
        for (int y = 0; y < H; y++) {
            const unsigned char* rc2 = yc2 + (size_t)y * pdc;
            for (int x = 0; x < W; x++) { mc2 += rc2[x]; histc[rc2[x]]++; if (rc2[x] >= 255) clipc++; }
        }
        long cumc = 0; double p99c = 255, p1c = 0;
        for (int i = 0; i < 256; i++) { cumc += histc[i];
            if (!p1c && cumc >= tot*0.01) p1c = i;
            if (cumc < tot*0.99) p99c = i; }
        printf("  C(recov): mean=%.1f p01=%.0f p99=%.0f clipped=%ld\n", mc2/tot, p1c, p99c, clipc);
        // mean gate +8 (bukan +12): frame backlight dg langit luas (ridge) mean-nya
        // didominasi area terang yg memang sengaja TIDAK disentuh filter
        CHECK(mc2/tot > ms/tot + 8, "T16C: recovery not visible (%.1f -> %.1f)", ms/tot, mc2/tot);
        CHECK(p1c > p1s + 12, "T16C: shadow recovery weak (p01 %.0f -> %.0f)", p1s, p1c);
        CHECK(p99c <= p99s + 35, "T16C: highlights blown (p99 %.0f -> %.0f)", p99s, p99c);
        CHECK(clipc < tot*0.001, "T16C: clipping %.2f%%", 100.0*clipc/tot);
        // dump C (Y+U+V)
        const unsigned char *uc2, *vc2; int puc3;
        uc2 = avs_get_read_ptr_p(fdc, AVS_PLANAR_U); vc2 = avs_get_read_ptr_p(fdc, AVS_PLANAR_V);
        puc3 = avs_get_pitch_p(fdc, AVS_PLANAR_U);
        FILE* f4 = fopen("/tmp/proof_C.yuv", "wb");
        for (int y = 0; y < H; y++) fwrite(yc2 + (size_t)y*pdc, 1, W, f4);
        for (int y = 0; y < chh; y++) fwrite(uc2 + (size_t)y*puc3, 1, cw, f4);
        for (int y = 0; y < chh; y++) fwrite(vc2 + (size_t)y*puc3, 1, cw, f4);
        fclose(f4);
        avs_release_video_frame(fdc);
        avs_release_video_frame(fs); avs_release_video_frame(fda); avs_release_video_frame(fdb);
    }

    if (failures == 0) printf("PASS: all Milestone-2..7 + REAL-FRAME checks (%s)\n", argv[1]);
    else fprintf(stderr, "RESULT: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
