# Algorithm Design Document

## HDRAGC-Next untuk AviSynth+

**Project:** HDRAGC-Next
**Target Host:** AviSynth+
**Mode:** Native AviSynth+ filter/plugin
**Backend awal:** CPU SIMD
**Backend opsional nanti:** GPU compute
**Fungsi utama:** Adaptive shadow brightening / local tone mapping untuk video SDR dengan kontrol banding, halo, temporal flicker, dan adaptasi spasial.

**Versi 2.2** — sinkronisasi penuh dengan implementasi (Milestone 1–7 terverifikasi runtime, 15 kelompok tes PASS, ~66 fps @ 640x480).
Changelog v2.2: (1) formula kurva = varian endpoint-preserving (implementasi), (2) LUT wajib interpolasi linear, (3) metrik scene cut = percentile-delta 5-titik (histogram intersection scale-blind ditolak empiris), (4) unit chroma = fraksi full-scale (0.5), (5) status milestone M1–M6 tervalidasi.

---

## 1. Tujuan Desain

Membuat filter AviSynth+ bernama:

```avs
HDRAGCNext()
```

yang mampu:

1. Mencerahkan area shadow secara adaptif — **termasuk adaptasi spasial lokal**.
2. Menjaga highlight agar tidak blown out.
3. Mengurangi banding dengan pipeline internal high bit-depth + dithering.
4. Menghindari halo dengan dekomposisi base-detail.
5. Mengurangi flicker antar frame dengan temporal smoothing.
6. Deteksi scene cut yang robust terhadap flash dan fade.
7. Tetap realistis untuk dijalankan di AviSynth+, baik CPU maupun nanti GPU-assisted.

---

## 2. Scope Awal

### 2.1. Fokus utama

Target awal adalah plugin AviSynth+ yang:

- Berjalan sebagai filter native C++.
- Mend input YUV planar.
- Memproses luma secara lokal dan adaptif (global curve + modulasi spasial).
- Memiliki temporal stabilization sederhana namun efektif.
- Bisa output 8-bit, 10-bit, atau 16-bit.
- Memiliki mode debug untuk development.

### 2.2. Bukan fokus awal

Untuk versi pertama, beberapa hal ini belum menjadi prioritas:

- HDR input PQ/HLG.
- Tone mapping HDR10 penuh.
- Motion-compensated temporal processing.
- GPU backend penuh.
- RGB pipeline internal penuh.
- AI/ML inference.

---

## 3. Kebijakan Format Video di AviSynth+

### 3.1. Input yang didukung

Versi awal disarankan menerima:

- YUV planar.
- Subsampling:
- YUV420
- YUV422
- YUV444
- Bit depth:
- 8-bit
- 10-bit
- 12-bit
- 14-bit
- 16-bit

Rekomendasi kualitas terbaik:

```avs
ConvertToYUV420P16()
```

atau jika ingin kualitas chroma lebih baik:

```avs
ConvertToYUV444P16()
```

Namun untuk performa, YUV420P16 sudah cukup baik untuk banyak kasus.

### 3.2. Input RGB

Untuk versi awal, input RGB tidak perlu diproses langsung secara internal.

Jika input RGB:

- Filter bisa menolak dengan pesan jelas, atau
- Wrapper script melakukan konversi dulu ke YUV.

Contoh wrapper:

```avs
function HDRAGCNextAuto(clip c, float "strength", float "temporal", int "output_bits")
{
    strength    = Default(strength, 0.8)
    temporal    = Default(temporal, 0.85)
    output_bits = Default(output_bits, 0)

    c = c.IsYUV() ? c : c.ConvertToYUV420P16(matrix="709")

    c.HDRAGCNext( \
        strength=strength, \
        temporal=temporal, \
        output_bits=output_bits \
    )
}
```

Alasan:

- YUV lebih natural untuk luma/chroma processing.
- Lebih murah daripada CIELAB.
- Lebih mudah menjaga kompatibilitas AviSynth+.
- Tidak perlu konversi color space perseptual yang berat pada tahap awal.

---

## 4. Nama Filter dan Interface AviSynth+

### 4.1. Nama filter

```avs
HDRAGCNext
```

### 4.2. Signature awal yang disarankan

```avs
HDRAGCNext(
    clip,
    float   "strength",
    float   "protect_highlights",
    float   "black_point",
    float   "white_point",
    bool    "auto_points",
    float   "temporal",
    int     "temporal_radius",
    string  "temporal_mode",
    float   "scene_cut",
    float   "scene_cut_low",
    int     "scene_cooldown",
    float   "detail_gain",
    float   "saturation",
    bool    "auto_saturation",
    string  "chroma_mode",
    float   "luma_ratio_mix",
    float   "chroma_softknee",
    int     "levels",
    float   "local_mix",
    float   "shadow_threshold",
    float   "mask_gamma",
    int     "mask_level",
    int     "output_bits",
    string  "dither",
    string  "show",
    bool    "debug"
)
```

### 4.3. Contoh signature C++

Untuk `AddFunction`:

```cpp
env->AddFunction(
    "HDRAGCNext",
    "c"
    "[strength]f"
    "[protect_highlights]f"
    "[black_point]f"
    "[white_point]f"
    "[auto_points]b"
    "[temporal]f"
    "[temporal_radius]i"
    "[temporal_mode]s"
    "[scene_cut]f"
    "[scene_cut_low]f"
    "[scene_cooldown]i"
    "[detail_gain]f"
    "[saturation]f"
    "[auto_saturation]b"
    "[chroma_mode]s"
    "[luma_ratio_mix]f"
    "[chroma_softknee]f"
    "[levels]i"
    "[local_mix]f"
    "[shadow_threshold]f"
    "[mask_gamma]f"
    "[mask_level]i"
    "[output_bits]i"
    "[dither]s"
    "[show]s"
    "[debug]b",
    Create_HDRAGCNext,
    nullptr
);
```

---

## 5. Parameter Desain

### 5.1. Parameter inti

| Parameter | Tipe | Default | Fungsi |
| --- | ---: | ---: | --- |
| `strength` | float | 0.8 | Kekuatan shadow lift / tone curve |
| `protect_highlights` | float | 0.8 | Proteksi highlight agar tidak clipping |
| `black_point` | float | auto | Titik hitam sumber |
| `white_point` | float | auto | Titik putih sumber |
| `auto_points` | bool | true | Hitung black/white point otomatis |
| `temporal` | float | 0.85 | Kekuatan temporal smoothing |
| `temporal_radius` | int | 4 | Radius frame untuk analisis temporal |
| `temporal_mode` | string | `"iir"` | `"iir"` atau `"window"` |
| `scene_cut` | float | 0.35 | Threshold konfirmasi scene cut |
| `scene_cut_low` | float | 0.20 | Threshold "curiga" untuk hysteresis |
| `scene_cooldown` | int | 3 | Blokir deteksi cut setelah reset |
| `detail_gain` | float | 1.0 | Penguatan detail layer |
| `saturation` | float | 1.0 | Saturasi chroma |
| `auto_saturation` | bool | true | Saturasi adaptif berdasarkan luma |
| `chroma_mode` | string | `"luma_ratio"` | `"simple"` (legacy) atau `"luma_ratio"` (hue-preserving) |
| `luma_ratio_mix` | float | 0.7 | Blend kompensasi rasio luma lokal vs global |
| `chroma_softknee` | float | 0.9 | Awal soft clamp, sebagai fraksi dari headroom |
| `levels` | int | 4 | Jumlah level Laplacian pyramid |
| `local_mix` | float | 1.0 | 0 = global curve murni, 1 = fully local |
| `shadow_threshold` | float | 0.35 | Ambang kegelapan mask; ≤ 0 = auto dari p50 |
| `mask_gamma` | float | 2.0 | Kekerasan rolloff mask |
| `mask_level` | int | -1 | Level pyramid untuk mask; -1 = coarsest |
| `output_bits` | int | 0 | 0 = sama seperti input, atau 8/10/12/14/16 |
| `dither` | string | `"blue"` | `"none"`, `"ordered"`, `"blue"` |
| `show` | string | `"none"` | Debug view |
| `natural` | float | 0.0 | Anti-fauxHDR: 0 = off, 1 = natural penuh (cap lift per-pixel, saturasi netral, mask tajam) |
| `debug` | bool | false | Aktifkan log internal |

---

## 6. Arsitektur Plugin AviSynth+

### 6.1. Struktur kelas utama

```cpp
class HDRAGCNext : public GenericVideoFilter
{
public:
    HDRAGCNext(
        PClip _child,
        float strength,
        float protect_highlights,
        float black_point,
        float white_point,
        bool auto_points,
        float temporal,
        int temporal_radius,
        int temporal_mode,
        float scene_cut,
        float scene_cut_low,
        int scene_cooldown,
        float detail_gain,
        float saturation,
        bool auto_saturation,
        int chroma_mode,
        float luma_ratio_mix,
        float chroma_softknee,
        int levels,
        float local_mix,
        float shadow_threshold,
        float mask_gamma,
        int mask_level,
        int output_bits,
        int dither_mode,
        int show_mode,
        bool debug
    );

    virtual PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);
    virtual void __stdcall SetCacheHints(int cachehints, int frame_range);

private:
    VideoInfo out_vi;

    // User parameters
    Params params;

    // Temporal analyzer
    TemporalAnalyzer temporal_analyzer;

    // Internal processing helpers
    void ProcessLuma(...);
    void ProcessChroma(...);
    void ApplyDither(...);
};
```

### 6.2. Constructor

Tugas constructor:

1. Validasi input clip.
2. Pastikan input planar YUV.
3. Tentukan output pixel type.
4. Siapkan parameter internal.
5. Set cache hints jika temporal aktif.

Contoh logika:

```cpp
if (!vi.IsPlanar() || !vi.IsYUV())
    env->ThrowError("HDRAGCNext: input must be planar YUV.");

if (params.output_bits == 0)
    params.output_bits = vi.BitsPerComponent();

out_vi = vi;

switch (params.output_bits)
{
    case 8:
        out_vi.pixel_type = ConvertToYUVType8(vi.pixel_type);
        break;
    case 10:
        out_vi.pixel_type = ConvertToYUVType10(vi.pixel_type);
        break;
    case 16:
        out_vi.pixel_type = ConvertToYUVType16(vi.pixel_type);
        break;
    default:
        env->ThrowError("HDRAGCNext: output_bits must be 0, 8, 10, 12, 14, or 16.");
}
```

Catatan penting:

- Jika input YUV420P16 dan output_bits=8, output menjadi YUV420P8.
- Jika input YUV422P16 dan output_bits=10, output menjadi YUV422P10.
- Subsampling sebaiknya dipertahankan.

---

## 7. Pipeline Utama dalam `GetFrame()`

### 7.1. Alur besar

```text
GetFrame(n)
    |
    +-- child->GetFrame(n)
    |
    +-- ComputeFrameStats(n)
    |       |
    |       +-- downscale luma
    |       +-- histogram / percentile
    |       +-- average luma
    |       +-- scene cut score (v2, lihat §12)
    |
    +-- TemporalSmoothing(stats)
    |       |
    |       +-- IIR atau window
    |       +-- scene cut state machine (hysteresis + cooldown)
    |
    +-- DeriveToneCurve()
    +-- DeriveSpatialMaskParams()   // tau auto, lihat §10.5
    |
    +-- Allocate / reuse internal buffers
    |
    +-- ProcessLuma()
    |       |
    |       +-- convert Y to internal precision
    |       +-- build Laplacian pyramid
    |       +-- compute shadow mask dari base level terendah (§10.5)
    |       +-- tone map base layer dengan lift termodulasi mask
    |       +-- optional detail gain (termodulasi mask yang sama)
    |       +-- reconstruct Y
    |       +-- clamp
    |
    +-- ProcessChroma()
    |       |
    |       +-- saturation boost
    |       +-- clamp
    |
    +-- OutputBitDepthConversion()
    |       |
    |       +-- dither if needed
    |
    +-- return output frame
```

---

## 8. Strategi Format Internal

### 8.1. Rekomendasi utama

Gunakan internal processing:

- **16-bit integer** untuk pipeline utama, atau
- **32-bit float** untuk kualitas maksimal dan kemudahan curve math.

Untuk AviSynth+ CPU, rekomendasi praktis:

| Tahap | Format |
| --- | --- |
| Input | YUV P8/P10/P12/P14/P16 |
| Internal luma | float32 |
| Pyramid detail | float32 atau int16 biased |
| Curve LUT | float32 atau uint16 |
| Output | P8/P10/P12/P14/P16 |

Jika performa CPU menjadi prioritas, float32 bisa diganti fixed-point 16-bit nanti.

### 8.2. Alasan memilih float32 untuk prototype

- Lebih mudah menghindari banding.
- Lebih mudah implement tone curve.
- Lebih mudah debugging.
- Tidak perlu terlalu khawatir overflow saat pyramid reconstruction.

Kekurangan:

- Memory lebih besar.
- Lebih lambat jika tidak SIMD-optimal.

Namun untuk versi awal, correctness lebih penting.

---

## 9. Desain Dekomposisi Base-Detail untuk AviSynth+

### 9.1. Metode yang dipilih

Gunakan **Laplacian Pyramid**, bukan single-pass bilateral/guided filter.

Alasan:

- Lebih stabil untuk video.
- Lebih mudah dikontrol per frequency band.
- Lebih kecil risiko gradient reversal.
- Lebih cocok untuk implementasi CPU SIMD.
- Dapat diskalakan dengan resolusi.
- **Level teratasnya dipakai sebagai field luminance spasial untuk adaptive gain (§10.5) — tanpa biaya tambahan.**

### 9.2. Jumlah level

Default:

```avs
levels = 4
```

Aturan adaptif:

```cpp
levels = clamp(levels, 2, 6);

while (level_width > 16 && level_height > 16 && level < levels)
{
    build_next_level();
}
```

Untuk 1080p:

- Level 0: 1920x1080
- Level 1: 960x540
- Level 2: 480x270
- Level 3: 240x135
- Level 4: 120x68

Base layer bisa diambil dari level terendah atau hasil blur level terendah.

### 9.3. Representasi pyramid

Untuk setiap level:

```cpp
struct PyramidLevel
{
    int width;
    int height;
    std::vector<float> data;
};
```

Untuk detail layer, nilai bisa negatif. Jika memakai int16, gunakan bias:

```cpp
stored_value = detail + 32768;
```

Namun untuk prototype, gunakan float32:

```cpp
detail = current_level - upsampled_next_level;
```

---

## 10. Tone Mapping pada Base Layer

### 10.1. Target kurva

Kurva harus:

- Mengangkat shadow.
- Menjaga midtone.
- Melindungi highlight.
- Tidak memotong detail gelap secara agresif.
- Tidak membuat highlight clipping.
- **Tervalidasi monotonic** — tambahkan assertion saat build LUT (lihat §10.6).

### 10.2. Parameter kurva

Dari statistik frame:

```cpp
float black_point;
float white_point;
float shadow_gain;
float midtone_pivot;
float highlight_protect;
```

Jika `auto_points=true`:

- `black_point` diambil dari percentile rendah, misalnya 1%.
- `white_point` diambil dari percentile tinggi, misalnya 99%.
- `shadow_gain` ditentukan oleh `strength` dan rata-rata luma.

### 10.3. Bentuk kurva

Untuk versi awal, gunakan smooth sigmoid atau filmic-like curve.

Formula konseptual awal `y = y/(1+h·y)` **ditolak saat implementasi**: terlalu agresif (Y=235 → ~140, bukan "proteksi" lagi). Varian endpoint-preserving yang terimplementasi dan tervalidasi monotonic untuk s,p ∈ [0,1]:

```cpp
xn = clamp((x - black) / (white - black), 0, 1)
y  = xn * (1 + s*(1 - xn))              // shadow lift; s = strength
h  = p * smoothstep(0.7, 1.0, y)        // p = protect_highlights
y  = pow(y, 1 + h)                      // kompresi highlight lembut; y(1)=1 terjaga
```

Sifat: monotonic (komposisi fungsi monotonic), y(0)=0, y(1)=1 (tidak ada white loss), highlight hanya tersentuh di atas 0.7.

### 10.4. LUT tone curve

Untuk CPU, gunakan LUT:

```cpp
std::vector<float> tone_lut; // size 4096 atau 65536
```

Untuk 16-bit input:

- LUT 4096 entry cukup untuk prototype.
- Interpolasi linear antar entry.
- Atau LUT 65536 jika memory masih aman.

Keuntungan LUT:

- Cepat.
- Deterministic.
- Mudah debug.
- Bisa di-smooth secara temporal lewat parameter, bukan LUT mentah.

**Wajib interpolasi linear antar entry** (temuan implementasi M3): kuantisasi grid murni
memberi error hingga ±8 lsb @16-bit (0.5 langkah grid × 65535/4095), cukup untuk merusak
round-trip identitas. Interpolasi linear mereproduksi ramp linear secara eksak.

### 10.5. **Spatially-Adaptive Shadow Gain (REVISI A)**

Tone curve global mengangkat semua pixel gelap dengan kekuatan sama tanpa konteks spasial. Akibatnya: lifted blacks di scene gelap, shadow terangkat berlebihan di scene terang, dan objek terang di area gelap ikut tersentuh. Solusinya: **modulasi lift berdasarkan luminance spasial lokal**, menggunakan level teratas pyramid yang sudah dibangun.

#### 10.5.1. Sumber field spasial

Field luminance lokal diambil dari **base level terendah pyramid** (level `mask_level`, default = `levels`):

```text
1080p, levels=4  →  field 120x68
4K,   levels=5   →  field ~240x135
```

Cukup halus untuk tidak menimbulkan artefak blok, cukup lokal untuk membedakan shadow region dari objek terang di dalamnya. `mask_level` mengontrol resolusi field:

| `mask_level` | Arti |
| --- | --- |
| `-1` (default) | `levels` — coarsest, paling stabil |
| `levels - 1` | adaptasi lebih halus, sedikit lebih mahal |
| `1` | adaptasi paling lokal — berisiko visible tiling, tidak disarankan |

#### 10.5.2. Shadow mask

```cpp
// t: 0 = tidak gelap, 1 = sangat gelap
float t = clamp((tau - L_lp) / tau, 0.0f, 1.0f);

// power curve untuk rolloff halus
float shadow_mask = powf(t, mask_gamma);
```

dengan `tau` = `shadow_threshold`, `mask_gamma` default 2.0.

#### 10.5.3. Penerapan lift — aturan tanda asimetris

**Lift positif (shadow brightening) dimodulasi mask; lift negatif (highlight compression) diterapkan penuh tanpa modulasi:**

```cpp
float lift = ToneCurve(L) - L;

if (lift > 0.0f)
    lift *= mask_eff;      // mask_eff = 1 - local_mix + local_mix * shadow_mask
                           //   = interpolasi global ↔ lokal

B'(x,y) = clamp(L(x,y) + lift, 0.0f, 1.0f);
```

Properti yang didapat:

1. **Highlight protection tidak rusak.** Kompressi highlight terjadi di L tinggi, tempat mask ≈ 0. Kalau lift negatif ikut dimodulasi, proteksi hilang di area gelap. Aturan ini menjamin highlight compression selalu aktif.
2. **`local_mix = 0` = perilaku global murni** — backward compatible, berguna untuk A/B test.
3. **Scene terang otomatis aman:** mask ≈ 0 hampir di mana-mana → lift ≈ 0 → tidak ada lifted blacks.
4. **Objek terang di area gelap terjaga:** pixel objek punya L tinggi → `ToneCurve(L) − L ≈ 0` → tidak ikut terangkat.

Karena mask didefinisikan di field 120x68 dan base layer di-tone-map juga di level itu, **tidak perlu upsampling mask ke full res** — detail layer mengangkat frekuensi tinggi kembali saat rekonstruksi. Biaya tambahan di bawah 1% dari total pipeline.

#### 10.5.4. Auto threshold

Jika `shadow_threshold <= 0`:

```cpp
tau = clamp(p50, 0.20f, 0.45f);
```

- Scene gelap (p50 = 0.15) → tau = 0.20 → lift aktif luas.
- Scene normal (p50 = 0.35) → tau = 0.35.
- Scene terang (p50 = 0.55) → tau = 0.45 → lift hampir mati.

`tau` ikut di-temporal-smooth bersama parameter lain — smoothing di level parameter, bukan mask per-pixel.

#### 10.5.6. Mode `natural` (anti-fauxHDR, T17)

Pseudo-HDR/faux-HDR punya tanda matematis yang dapat dibatasi: lift tak terbatas
(hitam jadi abu-abu total), saturasi ikut dinaikkan di area yang diangkat, dan
fluktuasi delta lokal besar (glow di sekitar objek). `natural` ∈ [0,1] menerapkan
tiga batasan sekaligus:

1. **Lift cap per-pixel** terhadap luma asli: `cap = 0.20·(1−0.75·natural)`
   → natural=1 berarti lift maks ~0.05 (13 lsb @8bit, terukur T17).
2. **Kompensasi saturasi dinetralkan**: `g_sat ← 1 + (g_sat−1)·(1−natural)`.
3. **Mask_gamma ditarik ke 2.5** ∝ natural — lift terkonsentrasi di shadow terdalam.

Terukur pada frame wajah gelap: mean +12,4, max lift 13 lsb — restrained, tanpa haze.

#### 10.5.5. Auto points dengan guard band (temuan konten nyata, T16)

Stretch black-point = p01 **menghancurkan shadow di foto gelap**: seluruh pixel di
bawah p01 (yang di konten gelap justru mayoritas massa shadow) ter-crush ke 0 — midtone
naik tapi mean mandek, p01 jatuh (terukur: 30 → 6 pada foto malam 612×408).
**Fix terimplementasi**: guard band —
`black = max(0, p01 − 0.10)`, `white = min(1, p99 + 0.05)` — sehingga massa percentile
tidak ter-crush. Terukur ulang: p01 30 → 55 (lentera), 17 → 59 (grup underexposure).

**Stretch blending (temuan visual kelima)**: stretch penuh pada gambar narrow-range
secara matematis me-re-gamma seluruh tonal range → **haze khas gamma-naik** (terlihat
pada foto grup; tidak pernah muncul di tes sintetis ber-range lebar). Fix:
titik auto hanya ditarik 60% dari identity — `black_eff = (p01−0.10)·0.6`,
`white_eff = 1 − (1−(p99+0.05))·0.6`. Konsekuensi: jarak output antar-scene mengecil
(aserti cut-detection T10 dipindah ke region gelap kurva yang responsif).

**Konsekuensi penting untuk konten setengah-gelap**: dengan mask_gamma default 2.0,
lift efektif hanya di bayangan terdalam (piksel L=0.2: mask = 0.18). Untuk recovery
yang *terlihat* pada landscape mean 41–88, knob perlu dinaikkan — preset "recovery"
terverifikasi: strength 1.0, protect 0.9, tau 0.45, mask_gamma 1.2. Terukur (mean):
valley 41→70, river 66→79, ridge 88→98, lentera 54→84, grup 32→72 — tanpa clipping,
highlight dalam band. **Gerbang aserti konten campuran**: p01 naik, p99 dalam band
±35, 0 clipping, mean ≥ +2 (bukan +10 — mean frame dengan area terang luas memang
hampir diam saat filter bekerja lokal dengan benar).

### 10.6. Validasi kurva

Saat membangun LUT, assertion wajib:

```cpp
for (int i = 1; i < N; ++i)
    assert(tone_lut[i] >= tone_lut[i-1]);   // monotonic

assert(tone_lut[0] >= 0.0f && tone_lut[N-1] <= 1.0f);
```

Kurva non-monotonic menyebabkan inversion artefak yang mustahil di-debug di level pixel. Build LUT gagal → fallback ke kurva identity + log debug, bukan frame rusak.

---

## 11. Temporal Stabilization di AviSynth+

Ini bagian paling sensitif di AviSynth+, karena frame bisa diminta secara random access, tidak selalu berurutan.

### 11.1. Masalah utama

Jika kurva dihitung per frame tanpa smoothing:

- Flicker.
- Exposure pumping.
- Scene berubah sedikit, brightness berubah.
- Noise statistik histogram menyebabkan kurva tidak stabil.

### 11.2. Dua mode temporal yang disarankan

#### Mode 1: `temporal_mode="iir"`

Default.

Cocok untuk playback linear.

Menggunakan Infinite Impulse Response:

```cpp
SmoothedParams[n] = alpha * CurrentParams[n] + (1 - alpha) * SmoothedParams[n - 1]
```

Dengan:

```cpp
alpha = 1.0f - temporal;
```

Contoh:

```avs
temporal = 0.85
```

Maka:

```cpp
alpha = 0.15
```

Artinya parameter baru hanya berpengaruh 15%, sisanya dari frame sebelumnya.

Kelebihan:

- Sangat smooth.
- Murah.
- Natural untuk video.

Kekurangan:

- Bergantung pada frame sebelumnya.
- Perlu handling untuk random access.
- Perlu scene cut reset.
- **Output non-deterministik tergantung riwayat seek** — trade-off yang harus disadari eksplisit (deterministic mode tersedia via `window`).

#### Mode 2: `temporal_mode="window"`

Lebih robust untuk random access.

Parameter dihitung dari window frame:

```text
[n - radius, ..., n, ..., n + radius]
```

atau causal:

```text
[n - radius, ..., n]
```

Parameter akhir adalah rata-rata atau median dari parameter frame dalam window.

Kelebihan:

- Lebih deterministic.
- Tidak terlalu bergantung state internal.
- Lebih aman untuk seeking.

Kekurangan:

- Lebih berat.
- Harus request banyak frame.
- Bisa menyebabkan lag visual jika window terlalu besar.

Rekomendasi default:

```avs
temporal_mode = "iir"
temporal_radius = 4
```

---

## 12. Desain Temporal Analyzer

### 12.1. Statistik frame

Untuk setiap frame, hitung statistik dari luma yang sudah di-downscale.

Tujuan downscale:

- Mengurangi biaya.
- Mengurangi pengaruh noise.
- Cukup untuk exposure decision.

Contoh ukuran analisis:

```cpp
analysis_width = 256;
analysis_height = 144;
```

Atau proporsional:

```cpp
max_side = 256;
```

### 12.2. Data statistik

```cpp
struct FrameStats
{
    float avg_luma;
    float p01;
    float p05;
    float p50;
    float p95;
    float p99;
    float contrast;      // p95 - p05
    float hist[64];      // histogram 64-bin ternormalisasi (sum = 1)
    float scene_score;
};
```

### 12.3. **Scene Cut Scoring v2 (REVISI B)**

#### 12.3.1. Metrik terimplementasi: percentile-delta 5 titik

**Metrik v1 (histogram intersection, termasuk varian Gaussian-smeared σ=2) ditolak
empiris**: scale-blind — sebaran spike yang bergeser sedikit saja sudah disjoint,
sehingga fade +8 RGB/frame menghasilkan skor 0.53 (terdeteksi sebagai cut, temporal
bypass total). Dua iterasi perbaikan (smeared intersection, KS) juga gagal mencapai
band yang benar.

Metrik final — delta percentile, skala-nyata, robust:

```cpp
// p = {p01, p25, p50, p75, p99} ternormalisasi 0..1 (dari histogram luma frame)
d_shift = mean_over_5( min(1, |p_cur[i] - p_prev[i]| / 0.125) );  // 12.5% shift = 1.0
d_p50   = min(1, |p50_cur - p50_prev| / 0.25);
d_contrast = min(1, |contrast_cur - contrast_prev| / max(contrast_prev, 0.05));
scene_score = 0.6*d_shift + 0.25*d_contrast + 0.15*d_p50;
```

Hasil terkalibrasi (terverifikasi tes T9–T11, klip 3-tone via UnalignedSplice):

| Situasi | Skor | Keputusan |
|---|---|---|
| Fade +8 RGB/frame | ~0.15 | IIR normal, bukan cut ✓ |
| Cut A→B (3-tone bergeser penuh) | ~0.45 | Cut instan ✓ |
| Flash uniform 1 frame | 1.0 | Cut kedua arah (kembali ke state A persis, no pumping) ✓ |

Catatan analisis: dithitung pada resolusi penuh luma (bukan downscale 256×144) untuk
M5 — downscale analisis ditunda ke optimasi M7.

#### 12.3.2. Two-threshold hysteresis + state machine

Satu threshold tunggal diganti dua threshold + state machine:

```cpp
// state: NORMAL | SUSPECT, plus cooldown counter

if (score >= scene_cut)            // 0.35, threshold konfirmasi
{
    ResetTemporalState();
    cooldown = scene_cooldown;     // default 3
    state = NORMAL;
}
else if (score >= scene_cut_low)   // 0.20, threshold curiga
{
    if (state == SUSPECT)
    {
        // dua frame berturut-turut suspicious → konfirmasi cut
        ResetTemporalState();
        cooldown = scene_cooldown;
        state = NORMAL;
    }
    else
    {
        // spike tunggal (flash, subtitle, objek lewat):
        // adaptasi dipercepat TANPA reset penuh
        state = SUSPECT;
        effective_alpha = min(1.0f, alpha * 2.5f);
    }
}
else
{
    state = NORMAL;
}

if (cooldown > 0) { cooldown--; }
```

Perilaku yang dijamin:

| Situasi | Hasil |
| --- | --- |
| Scene cut asli | Reset penuh, instan (skor ≥ 0.35 biasanya langsung) |
| Flash 1 frame (kilat, kamera flash) | Adaptasi dipercepat sesaat, **tidak** reset → tidak pumping |
| Flash 2+ frame berturut-turut | Dianggap scene baru → reset |
| Fade in/out | Delta antar frame kecil → skor < 0.20 → tidak pernah terdeteksi sebagai cut → smoothing normal, fade natural |
| Kamera cut halus (dark→dark) | `d_hist` tetap tinggi karena distribusi berubah → terdeteksi |

#### 12.3.3. Cooldown

Setelah reset, deteksi cut berikutnya diblokir selama `scene_cooldown` frame (default 3). Mencegah double-reset pada cut yang diikuti frame transisi. Untuk konten dengan flashing cepat beruntun, user bisa menaikkannya.

#### 12.3.4. Interaksi dengan random access

**Terimplementasi (M5)**: state machine di dalam `TemporalAnalyzer` dengan
`std::map<int, FrameState>` + mutex; anchor = frame ber-cache terdekat < n; gap >
`radius·2` → warm start ulang (state cache menyimpan propagated IIR state, jadi anchor
tidak harus n−1). Mode `window` fully deterministic tanpa state.

State machine bergantung pada frame sebelumnya. Untuk mode `iir`: state disimpan per frame di `param_cache`, reset jika gap frame terlalu jauh (lihat §13). Untuk mode `window`: scene cut score dihitung per frame dalam window (stats per frame sudah di-cache), cut dikonfirmasi per-frame — deterministic, tidak bergantung urutan seek.

#### 12.3.5. Catatan tuning

- Jika terlalu sensitif (flash salah deteksi sebagai cut): naikkan `scene_cut` ke 0.45 dan/atau `scene_cut_low` ke 0.25.
- Jika scene cut tidak terdeteksi: turunkan `scene_cut` ke 0.30 dan pastikan `scene_cut_low` = `scene_cut - 0.15`.

---

## 13. Handling Random Access di AviSynth+

AviSynth+ tidak menjamin `GetFrame(n)` dipanggil berurutan. Karena itu filter temporal harus dirancang hati-hati.

### 13.1. Strategi cache parameter

Filter menyimpan cache parameter per frame:

```cpp
std::map<int, SmoothedCurveParams> param_cache;
std::mutex temporal_mutex;
```

Ketika `GetFrame(n)` dipanggil:

1. Kunci mutex.
2. Cek apakah `param_cache[n]` sudah ada.
3. Jika ada, gunakan.
4. Jika tidak ada:

- Hitung stats frame n.
- Cari parameter frame sebelumnya yang tersedia.
- Jika ada, lanjutkan smoothing dari sana.
- Jika tidak ada, gunakan stats frame n sebagai initial state.

### 13.2. Kebijakan jika frame sebelumnya tidak tersedia

Untuk mode `iir`:

- Jika `n == 0`, init dari frame 0.
- Jika `n > 0` tetapi `n-1` tidak ada di cache:
- Coba cari frame cache terdekat sebelumnya.
- Jika jarak terlalu jauh, reset.
- Jika jarak dekat, lakukan smoothing dari cache terdekat.

Contoh:

```cpp
int max_cache_gap = temporal_radius * 2;

if (n - previous_cached_frame > max_cache_gap)
{
    reset_state();
}
```

### 13.3. Cache hints

Agar AviSynth+ menyimpan frame sekitar, filter bisa memberi hint:

```cpp
void HDRAGCNext::SetCacheHints(int cachehints, int frame_range)
{
    if (cachehints == CACHE_RANGE)
    {
        frame_range = max(params.temporal_radius + 2, 8);
    }
}
```

Catatan:

- Ini bukan jaminan absolut, tetapi membantu cache AviSynth+.
- Jangan memaksa cache terlalu besar jika memory terbatas.

---

## 14. Proses Luma

### 14.1. Ambil plane Y

Dari `PVideoFrame`:

```cpp
const uint8_t* srcY = src->GetReadPtr(PLANAR_Y);
int srcPitchY = src->GetPitch(PLANAR_Y);
int srcWidthY = src->GetRowSize(PLANAR_Y);
int srcHeightY = src->GetHeight(PLANAR_Y);
```

### 14.2. Buat frame output

```cpp
PVideoFrame dst = env->NewVideoFrame(out_vi);

uint8_t* dstY = dst->GetWritePtr(PLANAR_Y);
int dstPitchY = dst->GetPitch(PLANAR_Y);
```

### 14.3. Konversi ke internal float

Contoh normalisasi:

```cpp
float x = (float)pixel / 65535.0f;
```

Untuk 8-bit:

```cpp
float x = (float)pixel / 255.0f;
```

Untuk limited range, perlu hati-hati.

Opsi:

1. Proses full range sederhana.
2. Detect limited range dan normalize ke 0..1.
3. Tambahkan parameter `full_range` atau `levels` nanti.

Untuk versi awal, bisa pakai normalisasi sederhana berdasarkan bit depth, lalu tambahkan parameter range nanti.

---

## 15. Laplacian Pyramid untuk Luma

### 15.1. Build pyramid

```cpp
pyramid[0] = current_luma_float;

for (int i = 1; i <= levels; ++i)
{
    PyramidLevel down = Downsample(pyramid[i - 1]);
    PyramidLevel up = Upsample(down, pyramid[i - 1].width, pyramid[i - 1].height);
    pyramid_detail[i - 1] = pyramid[i - 1] - up;
    pyramid[i] = down;
}
```

### 15.2. Downsample filter

Gunakan Gaussian sederhana, misalnya 5-tap:

```cpp
[1, 4, 6, 4, 1] / 16
```

Separable:

- Horizontal pass.
- Vertical pass.
- Subsample 2x.

### 15.3. Upsample filter

Gunakan interpolasi:

- Bilinear untuk prototype.
- Bicubic untuk kualitas lebih baik.
- Gaussian upsample agar lebih halus.

### 15.4. Tone map base layer dengan adaptive gain

Base layer adalah level terkecil. Tone mapping dilakukan **per-pixel di level ini**, dengan lift termodulasi shadow mask (§10.5):

```cpp
// untuk setiap pixel di base level:
float lift = ToneCurve(base[x]) - base[x];

if (lift > 0.0f)
    lift *= mask_eff[x];   // 1 - local_mix + local_mix * shadow_mask[x]

base[x] = clamp(base[x] + lift, 0.0f, 1.0f);
```

### 15.5. Optional detail gain — termodulasi mask yang sama

Detail gain menggunakan shadow mask yang sama (menggantikan mask ad-hoc):

```cpp
for (int i = 0; i < levels; ++i)
{
    pyramid_detail[i] *= DetailGainMask(i);   // per-level gain, mask-aware
}
```

Untuk mencegah halo:

- Jangan perkuat detail layer paling rendah terlalu besar.
- Batasi `detail_gain` default 1.0 sampai 1.2.
- Detail di area gelap sedikit diperkuat (kompensasi noise/mud), detail di area terang tidak disentuh.

```cpp
float local_detail_gain = 1.0f + (detail_gain - 1.0f) * shadow_mask;
```

Namun untuk versi awal, lebih aman:

```avs
detail_gain = 1.0
```

atau maksimal:

```avs
detail_gain = 1.1
```

### 15.6. Reconstruct

```cpp
PyramidLevel current = pyramid[levels];

for (int i = levels - 1; i >= 0; --i)
{
    current = Upsample(current, pyramid[i].width, pyramid[i].height);
    current += pyramid_detail[i];
}
```

Setelah reconstruct:

```cpp
current = clamp(current, 0.0f, 1.0f);
```

#### 15.7. Anti-ring detail rolloff (temuan konten nyata, T16)

Rekonstruksi mentah menghasilkan **ringing di tepi kontras tinggi** → clamp 0/1
menyebabkan black crush (p01 24 → 9) dan highlight clip (194 px) pada foto lentera.
Fix terimplementasi: detail dilemahkan mendekati batas —

```cpp
w = 1 - 0.7 * |2*b - 1|     // b = nilai rekonstruksi saat ini
reconstructed += detail * w;
```

Hasil: clipping → 0 px, crush residual −2 pada p01 (known limitation jujur).

---

## 16. Proses Chroma (REVISI C)

### 16.1. Masalah dengan pendekatan sederhana

Ketika shadow diangkat, warna tampak pudar sehingga chroma perlu dikompensasi. Namun pendekatan global sederhana (boost saturasi merata + hard clamp per komponen) punya tiga kelemahan:

1. **Kompensasi tidak sesuai perubahan luma aktual.** Pixel yang terangkat banyak dan pixel yang hampir tidak disentuh kurva mendapat saturasi boost yang sama — hasilnya kompensasi berlebihan di sebagian area dan kurang di area lain.
2. **Hue shift saat clipping.** Clamp independen U dan V di dekat batas rentang mengubah arah vektor chroma → patch desaturated dengan hue yang salah, khas di shadow yang saturated.
3. **Posterization.** Hard clamp menciptakan area flat pada gradient warna saturated yang ikut terangkat.

### 16.2. Prinsip revisi

1. Kompensasi berdasarkan **rasio luma aktual per pixel**, diambil dari field base level terendah yang sudah tersedia (§10.5) — praktis tanpa biaya tambahan.
2. Scaling chroma sebagai **vektor** (hue-preserving), bukan clamp komponen independen.
3. **Soft clamp berbasis headroom**, bukan hard clamp.

### 16.3. Chroma gain gabungan

```cpp
// g_sat : faktor global (logika auto_saturation yang sudah ada,
//         temporally smoothed, clamp [0.8, 1.5])
// r     : rasio luma lokal dari base level terendah (smooth field)
float eps = 16.0f / 65535.0f;    // cegah ledakan division di near-black
float r   = clamp((Y_out_base + eps) / (Y_in_base + eps), 0.8f, 1.5f);
float g   = g_sat * (1.0f - luma_ratio_mix + luma_ratio_mix * r);
```

- `r` dihitung dari **base level** (smooth field), bukan per-pixel luma → tidak ada amplifikasi noise, dan inherit stabilitas temporal dari parameter yang sudah di-smooth.
- Field `Y_in_base` dan `Y_out_base` di-upsample bilinear ke resolusi chroma asli.

### 16.4. Hue-preserving soft clamp

Untuk setiap pixel chroma, perlakukan chroma sebagai vektor:

```cpp
// UNIT: fraksi full-scale — saturasi penuh per komponen = 0.5
// (offset dari neutral dibagi maxval, BUKAN maxval/2; salah unit 2x membuat
//  m_max selalu lebih kecil -> clamp agresif permanen, temuan M6)
float du = (U - neutral) / maxval, dv = (V - neutral) / maxval;
float m  = sqrtf(du*du + dv*dv);
if (m < 1e-6f) { /* chroma netral, copy langsung */ }

float m_max = 1.2f * min(Y_out, 1.0 - Y_out);   // Y_out normalized 0..1
if (m_max < 0.05f) m_max = 0.05f;

float m_target = g * m;

// soft knee mulai di chroma_softknee * m_max (default 0.9)
float knee = chroma_softknee * m_max;
if (m_target > knee)
{
    float excess = m_target - knee;
    float room   = m_max - knee;
    m_target = knee + excess * room / (excess + room);   // kompresi halus
}
m_target = min(m_target, m_max);

// scaling vektor: hue terjaga persis
U_out = neutral + (du / m) * m_target;
V_out = neutral + (dv / m) * m_target;
```

Properti:

- Hue **tidak pernah berubah** — hanya magnitudo yang dibatasi.
- Transisi clamp halus (soft knee) → tidak ada posterization.
- Headroom dihitung dari `Y_out` aktual → warna saturated di highlight tetap punya ruang; warna di near-black dibatasi otomatis lebih ketat.

### 16.5. Subsampling dan biaya

- Chroma diproses di resolusi chroma asli (420 = seperempat resolusi luma).
- Field rasio luma berasal dari base level terendah (120x68 untuk 1080p), di-upsample bilinear — murah.
- Soft knee dapat dibuat branchless → SIMD-friendly.
- Biaya tambahan diperkirakan < 3% dari total pipeline.

### 16.6. Mode fallback

`chroma_mode="simple"` mengaktifkan perilaku lama (boost global + hard clamp) untuk A/B test dan kompatibilitas. Default: `"luma_ratio"`.

### 16.7. Clamp akhir (safety net)

Setelah soft clamp, tetap ada safety clamp ke rentang bit depth (seharusnya hampir tidak pernah tersentuh). Jika tersentuh di banyak pixel, turunkan `chroma_softknee` ke 0.85 atau turunkan `saturation`.

---

## 17. Output Bit Depth dan Dithering

### 17.1. Output bits

Jika:

```avs
output_bits = 0
```

Maka output mengikuti input.

Jika:

```avs
output_bits = 8
```

Maka output 8-bit.

Jika:

```avs
output_bits = 10
```

Maka output 10-bit.

### 17.2. Dithering

Dithering wajib jika turun bit depth.

Pilihan:

- `none`
- `ordered`
- `blue`

Default:

```avs
dither = "blue"
```

Untuk prototype awal, `ordered` lebih mudah.

Untuk kualitas lebih baik, gunakan tiled blue noise 64x64 atau 128x128.

### 17.3. Jika output 16-bit

Jika output 16-bit:

- Dithering tidak wajib.
- Bisa langsung quantize atau round.

Contoh:

```cpp
out16 = (uint16_t)roundf(value * 65535.0f);
```

### 17.4. Jika output 8-bit

```cpp
float noise = GetBlueNoise(x, y);            // amplitudo ~1 LSB target
float value_dithered = value + noise / 65535.0f;
out8 = (uint8_t)roundf(value_dithered * 255.0f);
```

---

## 18. Debug Modes

Parameter `show` sangat penting untuk tuning.

### 18.1. Mode yang disarankan

| `show` | Output |
| --- | --- |
| `"none"` | Output final |
| `"base"` | Base layer setelah tone mapping |
| `"detail"` | Detail layer |
| `"mask"` | Shadow mask adaptif (di-upsampling untuk visualisasi) |
| `"lift"` | Visualisasi `ToneCurve(L) − L` setelah modulasi mask |
| `"curve"` | Visualisasi curve |
| `"hist"` | Histogram sederhana |
| `"stats"` | Text stats, jika memungkinkan |

Contoh:

```avs
HDRAGCNext(show="base")
HDRAGCNext(show="detail")
HDRAGCNext(show="mask")
```

Untuk AviSynth+, visualisasi curve bisa dibuat sebagai overlay gambar kecil pada frame, atau simpan sebagai file debug jika `debug=true`.

---

## 19. Thread Safety dan Memory

### 19.1. Thread safety

AviSynth+ dapat memanggil `GetFrame()` dari beberapa thread.

Karena filter ini memiliki temporal cache, gunakan:

```cpp
std::mutex temporal_mutex;
```

Namun jangan memegang mutex terlalu lama saat proses berat.

Strategi:

1. Kunci mutex hanya untuk membaca/menulis cache parameter.
2. Lepaskan mutex saat proses pixel berat.
3. Gunakan buffer lokal per thread jika memungkinkan.

### 19.2. Memory buffer

Hindari alokasi besar setiap frame.

Gunakan reusable buffer:

```cpp
struct ProcessingBuffers
{
    std::vector<float> luma_float;
    std::vector<PyramidLevel> pyramid;
    std::vector<PyramidLevel> detail;
    std::vector<float> tone_lut;
    std::vector<float> shadow_mask;   // field coarsest, lihat §10.5
};
```

Jika AviSynth+ memanggil dari banyak thread, pilihan:

- Satu buffer global + mutex, sederhana tetapi bisa bottleneck.
- Thread-local buffer, lebih cepat tetapi lebih kompleks.
- Buffer pool, keseimbangan terbaik.

Untuk versi awal, satu buffer global dengan mutex bisa diterima, lalu optimasi nanti.

---

## 20. Performa

### 20.1. Target awal

Target realistis:

- 1080p YUV420P16, CPU modern:
- prototype: 5–15 FPS
- optimized SIMD: 20–60 FPS tergantung CPU
- 720p:
- lebih mudah real-time.

### 20.2. Optimasi CPU

Gunakan:

- SSE4.2
- AVX2
- Fast Gaussian downsample
- LUT tone mapping
- Hindari branch per pixel
- Proses row-based
- Gunakan pointer aligned jika memungkinkan

### 20.3. Optimasi analisis temporal

Jangan analisis full resolution.

Gunakan:

```cpp
analysis_size = 256x144
```

atau:

```cpp
analysis_size = 320x180
```

Histogram bisa 256 bin atau 1024 bin (scene cut v2 memakai 64-bin ternormalisasi untuk intersection distance).

### 20.4. GPU nanti

Jika nanti GPU:

- Vulkan compute shader.
- DirectCompute.
- CUDA.

Namun untuk AviSynth+, GPU backend harus hati-hati karena:

- Frame data berada di CPU memory.
- Perlu upload/download per frame.
- Latency bisa tinggi.
- Harus aman untuk banyak instance filter.

Jadi GPU sebaiknya fase lanjutan, bukan fondasi awal.

---

## 21. Contoh Penggunaan AviSynth+

### 21.1. Basic

```avs
LoadPlugin("HDRAGCNext.dll")

source = FFVideoSource("input.mp4")

source
ConvertToYUV420P16()
HDRAGCNext(
    strength=0.8,
    protect_highlights=0.8,
    temporal=0.85,
    temporal_mode="iir",
    scene_cut=0.35,
    output_bits=8,
    dither="blue"
)
```

### 21.2. Kualitas tinggi

```avs
LoadPlugin("HDRAGCNext.dll")

FFVideoSource("input.mp4")
ConvertToYUV444P16()
HDRAGCNext(
    strength=0.9,
    protect_highlights=0.9,
    temporal=0.9,
    temporal_radius=6,
    detail_gain=1.1,
    auto_saturation=true,
    levels=5,
    local_mix=1.0,
    shadow_threshold=0,
    output_bits=16,
    dither="none"
)
```

### 21.3. A/B test global vs local

```avs
# Global murni (perilaku tanpa revisi A)
HDRAGCNext(local_mix=0.0, output_bits=16)

# Fully local (default revisi)
HDRAGCNext(local_mix=1.0, output_bits=16)
```

### 21.4. Debug base layer / mask / lift

```avs
HDRAGCNext(show="base", output_bits=16)
HDRAGCNext(show="mask")
HDRAGCNext(show="lift")
```

### 21.5. Debug detail

```avs
HDRAGCNext(
    detail_gain=1.2,
    show="detail"
)
```

---

## 22. Wrapper Script yang Disarankan

Agar user tidak perlu bingung konversi format:

```avs
function HDRAGCNextAuto(
    clip c,
    float "strength",
    float "protect_highlights",
    float "temporal",
    int   "output_bits",
    string "dither",
    string "show"
)
{
    strength           = Default(strength, 0.8)
    protect_highlights = Default(protect_highlights, 0.8)
    temporal           = Default(temporal, 0.85)
    output_bits        = Default(output_bits, 0)
    dither             = Default(dither, "blue")
    show               = Default(show, "none")

    Assert(c.IsYUV(), "HDRAGCNextAuto: input must be YUV")

    c = c.BitsPerComponent() == 16 ? c : c.ConvertToYUV420P16()

    c.HDRAGCNext(
        strength=strength,
        protect_highlights=protect_highlights,
        temporal=temporal,
        temporal_mode="iir",
        scene_cut=0.35,
        scene_cut_low=0.20,
        scene_cooldown=3,
        output_bits=output_bits,
        dither=dither,
        show=show
    )
}
```

Pemakaian:

```avs
HDRAGCNextAuto(strength=0.85, output_bits=8)
```

---

## 23. Risiko Teknis dan Mitigasi

### 23.1. Flicker temporal

**Risiko:** Parameter kurva berubah terlalu cepat.

**Mitigasi:**

- Gunakan IIR smoothing.
- Analisis dari downscaled luma.
- Gunakan percentile, bukan min/max.
- Scene cut detection v2 (hysteresis + cooldown).

### 23.2. Random access AviSynth+

**Risiko:** Frame diminta tidak berurutan, state temporal kacau.

**Mitigasi:**

- Cache parameter per frame.
- Reset jika gap terlalu jauh.
- Sediakan `temporal_mode="window"` yang lebih deterministic.
- Jangan asumsikan frame selalu linear.

### 23.3. Memory besar

**Risiko:** Pyramid float32 membutuhkan memory besar.

**Mitigasi:**

- Batasi level.
- Gunakan buffer reuse.
- Untuk 4K, kurangi levels.
- Opsional turun ke 16-bit fixed point.

### 23.4. Halo

**Risiko:** Detail gain berlebihan.

**Mitigasi:**

- Default `detail_gain=1.0`.
- Batas maksimal `detail_gain=1.3`.
- Jangan tone map detail layer.
- Gunakan shadow mask untuk detail gain.

### 23.5. Banding

**Risiko:** Output 8-bit tanpa dither.

**Mitigasi:**

- Internal float32.
- Dithering saat turun bit depth.
- Jangan quantize langsung tanpa noise.

### 23.6. Performa lambat

**Risiko:** Full-resolution pyramid terlalu berat.

**Mitigasi:**

- Optimasi SIMD.
- Analisis temporal di resolusi kecil.
- Gunakan LUT.
- Kurangi level pyramid untuk resolusi besar.

### 23.7. Lifted blacks di scene gelap (REVISI A)

**Risiko:** Shadow lift global membuat area yang seharusnya hitam menjadi abu-abu.

**Mitigasi:**

- Shadow mask spasial dari base level terendah pyramid.
- Auto threshold dari p50 — scene terang otomatis hampir tidak terlift.
- `local_mix` bisa diturunkan untuk konten yang tidak cocok.

### 23.8. False scene cut pada flash / false negative pada fade (REVISI B)

**Risiko:** Spike satu frame mereset state temporal (pumping); fade terdeteksi sebagai cut.

**Mitigasi:**

- Tiga metrik ternormalisasi (histogram intersection, p50 delta, contrast delta).
- Two-threshold hysteresis: spike tunggal = adaptasi dipercepat tanpa reset.
- Cooldown mencegah double-reset.
- Fade delta per frame kecil → tidak pernah terdeteksi sebagai cut.

### 23.9. Kurva non-monotonic (REVISI §10.6)

**Risiko:** Tone curve ad-hoc menghasilkan inversi (pixel lebih gelap setelah filter).

**Mitigasi:**

- Assertion monotonic saat build LUT.
- Fallback ke identity + log debug, bukan frame rusak.

### 23.10. Hue shift dan clipping chroma (REVISI C)

**Risiko:** Shadow saturated yang diangkat mengalami hue shift (clamp independen U/V) atau posterization (hard clamp).

**Mitigasi:**

- Kompensasi berbasis rasio luma lokal, bukan boost global merata (§16.3).
- Scaling vektor chroma — hue terjaga persis (§16.4).
- Soft clamp headroom-aware — transisi halus, tidak ada posterization.
- Mode fallback `chroma_mode="simple"` untuk A/B test.

---

## 24. Test Plan

### 24.1. Test banding

Gunakan klip dengan gradien gelap:

- Langit malam.
- Fog.
- Fade to black.
- Shadow wall.

Periksa:

- Tidak ada stepping.
- Dither tidak terlalu visible.
- Gradien tetap smooth.

### 24.2. Test halo

Gunakan klip dengan edge kontras tinggi:

- Teks terang di background gelap.
- Lampu malam.
- Siluet.
- Objek dengan rim light.

Periksa:

- Tidak ada glow berlebihan.
- Edge tetap tajam.
- Tidak ada gradient reversal.

### 24.3. Test temporal

Gunakan klip:

- Static camera dengan perubahan cahaya kecil.
- Walking scene.
- Night scene dengan noise.
- Scene cut terang-gelap.

Periksa:

- Tidak flicker.
- Exposure transition smooth.
- Scene cut langsung reset.

### 24.4. Test chroma (REVISI C)

Gunakan klip:

- Shadow berwarna saturated (neon sign, lampu warna di malam hari).
- Skin tone di lighting beragam (daylight, tungsten, low-key).
- Warna saturated di area gelap yang diangkat (poster, kostum, stage lighting).
- Gradient warna saturated (langit senja) untuk deteksi posterization.

Periksa:

- Warna tidak pudar setelah lift.
- **Hue tidak bergeser** di region yang mendekati batas — sample pixel sebelum/sesudah di area saturated, toleransi Δhue kecil (mis. < 3°).
- Tidak ada posterization / area flat pada gradient saturated.
- Skin tone natural di semua kondisi lighting.
- A/B `chroma_mode="simple"` vs `"luma_ratio"`: mode baru harus menang jelas di shadow saturated, dan setara di konten biasa.

### 24.5. Test seek

Di editor/player:

- Seek maju.
- Seek mundur.
- Loncat jauh.
- Ulangi frame range.

Periksa:

- Filter tidak crash.
- Tidak deadlock.
- Output tetap valid.
- Cache tidak meledak.

### 24.6. Test spatially-adaptive gain (REVISI A)

Gunakan klip:

- **Bright scene dengan shadow** (siang hari, area teduh): shadow naik moderat, highlight tidak berubah.
- **Dark scene penuh** (night interior): lift merata tapi tidak sampai lifted blacks — area yang memang hitam tetap gelap relatif terhadap lingkungannya.
- **Objek terang di area gelap** (lampu malam, TV menyala di ruangan gelap): objek tidak ikut terangkat, tidak ada glow di sekitarnya.

Periksa:

- `show="mask"`: mask ≈ 0 di area terang, ≈ 1 di shadow region.
- `show="lift"`: lift positif hanya di area mask tinggi; di area highlight lift ≈ 0 (kompressi negatif tetap ada).
- A/B `local_mix=0.0` vs `1.0`: perbedaan harus terlihat jelas di bright scene, halus di dark scene.

### 24.7. Test scene cut v2 (REVISI B)

| Klip | Ekspektasi |
| --- | --- |
| Flash 1 frame di scene statis | Tidak reset; brightness kembali tanpa pumping |
| Flash 2+ frame berturut-turut | Dianggap scene baru → reset |
| Fade to black 30 frame | Tidak terdeteksi sebagai cut; transisi exposure smooth |
| Cut terang↔gelap berurutan | Reset instan tiap cut |
| Dark-to-dark cut (komposisi beda) | Terdeteksi via `d_hist` |
| Subtitle muncul di scene gelap | Tidak terdeteksi sebagai cut (p50 stabil, histogram intersection tinggi) |

### 24.8. Test monotonicity kurva

- Jalankan seluruh test suite dengan assertion LUT aktif.
- Sengaja set parameter ekstrem (`strength=2.0`, `protect_highlights=1.0`): tidak boleh ada assertion failure; jika terjadi, fallback identity tercatat di log.

---

## 25. Milestone Implementasi

### Milestone 1: Skeleton plugin

Target:

- Plugin load di AviSynth+.
- Filter terdaftar.
- Validasi pixel type.
- Pass-through frame berhasil.

Output:

```avs
HDRAGCNext()
```

masih tidak mengubah gambar.

### Milestone 2: Internal precision dan pass-through aman

Target:

- Konversi input ke float32.
- Proses Y.
- Copy U/V.
- Konversi balik ke output bits.
- Dithering dasar.

Hasil:

- Gambar hampir sama.
- Tidak banding.
- Tidak crash.

### Milestone 3: Tone curve global

Target:

- Hitung statistik frame.
- Hitung black/white point.
- Buat tone curve + assertion monotonicity (§10.6).
- Terapkan pada luma tanpa pyramid.

Hasil:

- Shadow naik.
- Masih mungkin ada halo, tapi fungsi utama sudah terlihat.

### Milestone 4: Laplacian pyramid + spatially-adaptive gain

Target:

- Build pyramid.
- Shadow mask dari base level terendah (§10.5).
- Tone map base layer dengan lift termodulasi mask (aturan asimetris).
- Reconstruct.
- Debug `show="base"`, `show="mask"`, `show="lift"`.

Hasil:

- Halo berkurang.
- Detail lebih terjaga.
- Lifted blacks terkendali; objek terang di area gelap terjaga.

### Milestone 5: Temporal smoothing + scene cut v2

Target:

- Tiga metrik scene score ternormalisasi (§12.3.1).
- Hysteresis state machine + cooldown (§12.3.2).
- IIR parameter smoothing.
- Cache parameter.
- Mode `temporal_mode="window"` opsional.

Hasil:

- Flicker berkurang signifikan.
- Exposure stabil.
- Flash tidak memicu pumping; fade tidak salah deteksi.

### Milestone 6: Chroma — luma-ratio saturation + hue-preserving soft clamp

Target:

- `g_sat` global (manual + auto_saturation), temporally smoothed.
- Rasio luma lokal dari base level terendah (§16.3).
- Scaling vektor chroma + soft clamp headroom-aware (§16.4).
- Mode fallback `chroma_mode="simple"`.

Hasil:

- Warna tidak pudar setelah lift.
- Hue terjaga, tidak ada posterization di shadow saturated.

### Milestone 7: Optimasi SIMD

Target:

- AVX2.
- Row-based processing.
- Reusable buffers.
- Faster downsample/upsample.

Hasil:

- FPS naik signifikan.

### Milestone 8: Debug dan QA

Target:

- `show` modes lengkap.
- Debug stats.
- Logging optional.
- Test suite klip (termasuk §24.6–§24.8).

Hasil:

- Mudah tuning dan reproduksi bug.

---

## 26. Keputusan Desain Penting

1. **Gunakan YUV, bukan CIELAB.**

- Lebih cepat.
- Lebih kompatibel.
- Lebih mudah di AviSynth+.

2. **Proses utama pada kanal Y.**

- Luma adalah komponen terpenting untuk shadow lifting.
- Chroma dikompensasi hue-preserving berbasis rasio luma lokal + soft clamp headroom-aware (§16).

3. **Gunakan Laplacian Pyramid.**

- Lebih aman daripada single edge-aware filter.
- Lebih mudah dikontrol.
- Lebih cocok untuk video.
- Level teratasnya dobel sebagai field spasial untuk adaptive gain — gratis.

4. **Gunakan float32 internal untuk prototype.**

- Kualitas maksimal.
- Mudah debugging.
- Bisa diturunkan ke fixed-point nanti.

5. **Temporal smoothing memakai parameter curve, bukan pixel.**

- Lebih murah.
- Lebih stabil.
- Tidak perlu motion estimation.

6. **Scene cut detection pakai hysteresis dua-threshold + cooldown.**

- Flash tunggal ≠ cut (tidak reset, adaptasi dipercepat saja).
- Fade ≠ cut.
- Cut asli reset instan.

7. **Shadow lift adaptif spasial dengan aturan asimetris.**

- Lift positif dimodulasi shadow mask (dari pyramid, tanpa biaya).
- Lift negatif (highlight compression) selalu penuh — proteksi highlight tidak pernah rusak.
- `local_mix=0` memberi perilaku global murni sebagai fallback.

8. **Kurva wajib tervalidasi monotonic saat build LUT.**
9. **Dithering wajib saat output bit-depth turun.**

- Terutama output 8-bit.

10. **GPU jangan jadi fondasi awal.**

    - CPU SIMD dulu.
    - GPU sebagai backend opsional nanti.

11. **Chroma: hue-preserving, headroom-aware.**
    - Kompensasi mengikuti rasio luma aktual per pixel (field base pyramid), bukan boost global merata.
    - Scaling vektor chroma menjaga hue; soft clamp headroom-aware menghindari posterization.
    - Mode fallback `chroma_mode="simple"` untuk A/B test dan kompatibilitas.

---

## 27. Kesimpulan

Untuk AviSynth+, desain **HDRAGC-Next** paling aman dan praktis adalah:

- Filter native C++ AviSynth+.
- Input YUV planar.
- Proses luma dalam float32 internal.
- Dekomposisi base-detail menggunakan Laplacian Pyramid.
- Tone mapping adaptif pada base layer, dengan **shadow lift termodulasi field luminance spasial dari level pyramid terendah** (aturan asimetris: lift positif termodulasi, kompressi highlight selalu penuh).
- **Scene cut scoring dari tiga metrik ternormalisasi** (histogram intersection, p50 delta, contrast delta) dengan hysteresis dua-threshold dan cooldown.
- Parameter kurva di-smooth secara temporal dengan IIR.
- Kompensasi chroma hue-preserving berbasis rasio luma lokal, dengan soft clamp headroom-aware (warna saturated di shadow tidak clipping, hue tidak bergeser).
- Output dengan dithering saat bit depth diturunkan.
- Validasi monotonicity kurva saat build LUT.
- Optimasi CPU SIMD dulu, GPU nanti.

Dengan pendekatan ini, HDRAGC-Next bisa menjadi pengganti HDRAGC lama yang jauh lebih modern, lebih stabil, lebih robust terhadap flash/fade, benar-benar adaptif secara spasial, dan lebih cocok untuk workflow AviSynth+.

---

## 28. Catatan Penilaian Desain

**Sebagai algoritma:** ~9/10 — 17 kelompok tes runtime PASS (T1–T15 sintetis + T16 lima foto asli + T17 natural), dengan tujuh temuan konten-nyata terdokumentasi dan ter-fix (guard band §10.5.5, anti-ring §15.7 + levels adaptif + anti-ring gated, tau default, stretch blending, gerbang aserti adaptif, mode natural §10.5.6).

Peningkatan dari versi awal (6,5–7/10):

- Klaim "local tone mapping" kini benar-benar terpenuhi lewat spatially-adaptive gain berbiaya ~nol (Revisi A).
- Scene cut scoring well-defined (semua komponen 0..1) dan robust terhadap flash/fade (Revisi B).
- Penanganan chroma hue-preserving dan headroom-aware — kompensasi mengikuti perubahan luma aktual per pixel, hue terjaga, tidak ada posterization (Revisi C).

Sisa yang masih bisa ditingkatkan di versi berikutnya:

1. Penanganan limited/full range masih ditunda — untuk konten nyata perlu dideteksi dari `VideoInfo` + parameter eksplisit.
2. Mode `iir` tetap non-deterministik tergantung riwayat seek — trade-off yang disadari; `window` tersedia sebagai alternatif deterministic.
3. Skala spasial masih satu tingkat (satu mask level) — multi-scale gain map bisa dieksplorasi nanti.
4. Headroom chroma (`1.2·min(y,1−y)`) layak dijadikan parameter `chroma_headroom`; auto-percentile remap pada konten narrow-range sangat agresif (by design p01/p99 stretch) dan mendominasi clamp — perilaku yang disadari, perlu tuning konten nyata.
5. M7 selesai dasar: `-O3 -mavx2` flags, `show` modes, blue noise, LUT member reuse, `SetCacheHints(CACHE_GENERIC)` (return **int**). Sisa: intrinsik AVX2 manual, downscale analisis 256x144.
6b. Fix haze (temuan visual panel wajah): levels ADAPTIF (base >= 1/8 min-dim — frame kecil tak lagi di-paksa 4 level sampai base 22px), anti-ring GATED (rolloff hanya utk |detail|>0.1; tekstur rambut/kulit utuh). Keduanya menjawab "haze makin kelihatan" pada konten asli.
6. Raw-source input (`RawSourceYV12`) memungkinkan uji konten nyata — **bug pitch-align dan U/V swap di sanalah yang membuka tiga temuan T16**; pipeline uji end-to-end dengan konten nyata adalah komponen wajib.
