// vshost.c - M2 smoke test for the VapourSynth adapter (VSScript API R81+).
// Verifies: (a) strength=0 -> byte-identical passthrough on BlankClip
// (YUV420P8/P16, YUV444P16); (b) RGB24 input is rejected.
// Build: gcc -O1 -o vshost vshost.c -I<vs include> -lvsscript -lvapoursynth
#include <stdio.h>
#include <string.h>
#include <VSScript4.h>
#include <VapourSynth4.h>

static int failures = 0;
#define CK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <hdragcnext_vs.so|dll>\n", argv[0]); return 2; }
    const VSSCRIPTAPI* ss = getVSScriptAPI(VSSCRIPT_API_VERSION);
    if (!ss) { fprintf(stderr, "FAIL: getVSScriptAPI\n"); return 1; }
    const VSAPI* vsapi = ss->getVSAPI(VAPOURSYNTH_API_VERSION);
    if (!vsapi) { fprintf(stderr, "FAIL: getVSAPI\n"); return 1; }

    const char* fmt[] = { "YUV420P8", "YUV420P16", "YUV444P16" };
    for (int i = 0; i < 3; i++) {
        char script[2048];
        snprintf(script, sizeof(script),
            "import vapoursynth as vs\n"
            "core = vs.core\n"
            "core.std.LoadPlugin(path='%s')\n"
            "src = core.std.BlankClip(width=64, height=64, length=3, format=vs.%s)\n"
            "pas = core.hlgef.HDRAGCNext(src, gain=0.0)\n"
            "diff = core.std.Expr([src, pas], 'x y - abs')\n"
            "core.std.PlaneStats(diff).set_output()\n", argv[1], fmt[i]);
        VSScript* se = ss->createScript(NULL);
        if (ss->evaluateBuffer(se, script, "vshost") != 0) {
            fprintf(stderr, "FAIL %s: %s\n", fmt[i], ss->getError(se));
            failures++; ss->freeScript(se); continue;
        }
        VSNode* node = ss->getOutputNode(se, 0);
        if (!node) {
            fprintf(stderr, "FAIL %s: no output node (%s)\n", fmt[i], ss->getError(se));
            failures++; ss->freeScript(se); continue;
        }
        const VSFrame* f = vsapi->getFrame(0, node, NULL, 0);
        const VSMap* m = vsapi->getFramePropertiesRO(f);
        double mxv = vsapi->mapGetFloat(m, "PlaneStatsMax", 0, NULL);
        CK(mxv == 0.0, "%s: strength=0 not byte-identical (max diff %.1f)", fmt[i], mxv);
        printf("%s: identity %s\n", fmt[i], mxv == 0.0 ? "OK" : "FAILED");
        vsapi->freeFrame(f);
        vsapi->freeNode(node);
        ss->freeScript(se);
    }
    {
        char script[2048];
        snprintf(script, sizeof(script),
            "import vapoursynth as vs\n"
            "core = vs.core\n"
            "core.std.LoadPlugin(path='%s')\n"
            "src = core.std.BlankClip(width=64, height=64, format=vs.RGB24)\n"
            "core.hlgef.HDRAGCNext(src).set_output()\n", argv[1]);
        VSScript* se = ss->createScript(NULL);
        int err = ss->evaluateBuffer(se, script, "vshost");
        CK(err != 0, "RGB24 input was NOT rejected");
        printf("RGB24 rejection %s\n", err ? "OK" : "FAILED");
        ss->freeScript(se);
    }
    if (!failures) printf("PASS: vshost smoke test\n");
    return failures ? 1 : 0;
}
