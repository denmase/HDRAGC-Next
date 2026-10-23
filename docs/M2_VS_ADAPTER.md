# M2: VapourSynth Adapter — RUNTIME-VERIFIED

`src/vs.cpp` registers `HDRAGCNext` with VapourSynth (API4; headers R81
vendored in `include/vs/`) linking the same `hdragc_core` as the AviSynth+
adapter. Parameter names, defaults and validation are identical to the AVS
interface (integer planar YUV 8-16 bit, 3 planes; RGB/float rejected).

## Runtime verification (VapourSynth R81, manylinux wheel)

    plugin loaded: ['HDRAGCNext']
    YUV420P8  identity max diff: 0
    YUV420P16 identity max diff: 0
    YUV444P16 identity max diff: 0
    RGB24 rejected OK: input must be integer planar YUV (8/10/12/14/16-bit)

strength=0 passthrough is byte-identical on all three formats.

## AVS regression after the VS-driven refactor

The VS frame pipeline required threading a request context through
FrameSrc.fetch -> processFrame -> GetState -> GetRaw (two-phase
arInitial/arAllFramesReady + requestFrameFilter/getFrameFilter; the AVS
adapter passes nullptr). After that change: full suite T1-T17 PASS (rc=0)
and all four T16 dumps byte-identical vs the pre-refactor baseline build.

## Three API4 pitfalls fixed (diagnosed from exact symptoms)

1. configPlugin silently rejects apiVersion newer than the core
   -> request VS_MAKE_VERSION(4, 0).
2. API4 clip argument type is "vnode" in signature strings.
3. Synchronous getFrame is forbidden inside the frame pipeline
   -> two-phase activation pattern with request/getFrameFilter.
