# M1: Dual-Interface Core Extraction

Algoritma diekstrak dari wrapper AviSynth+ menjadi core API-agnostic
(`src/core.h` / `src/core.cpp`); adapter AVS tipis di `src/avisynth.cpp`.
Gerbang: T1-T17 PASS + keempat dump T16 byte-identik terhadap build
pre-refactor (bukti refactor murni, tanpa perubahan perilaku).

M2 berikutnya: adapter VapourSynth (`src/vs.cpp`) link target
`hdragc_core` yang sama + `test/vshost.c`.
