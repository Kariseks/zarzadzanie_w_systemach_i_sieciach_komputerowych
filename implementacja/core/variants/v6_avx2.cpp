// ============================================================================
//  V6 – reczne SIMD AVX2 (intrinsics) na splocie rozdzielnym
//  V8 – V6 + przeliczanie zamiast przechowywania (KernelInfo::recompute):
//       przy skalowaniu GlobalMax pierwszy przebieg liczy TYLKO maksimum
//       (bez zapisu modulu), drugi liczy wszystko od nowa i od razu zapisuje
//       bajty.  Znika bufor float (4 B/piksel zapisu + 4 B/piksel odczytu),
//       za cene dwukrotnych obliczen.  Oplaca sie, gdy zadanie jest
//       ograniczone przepustowoscia pamieci (duze obrazy, wiele watkow).
//
//  Przebieg poziomy: 16 pikseli na iteracje (uint8 -> int16, mnozenie
//  _mm256_mullo_epi16 przez stale maski).  Przebieg pionowy: 8 pikseli na
//  iteracje (int16 -> int32, _mm256_mullo_epi32).  Modul:
//    L2  – _mm256_sqrt_ps, gdy modul^2 < 2^24 (wynik dokladny jak w V0),
//          inaczej 2 x _mm256_sqrt_pd (double),
//    L1  – |Gx| + |Gy|,  Linf – max(|Gx|, |Gy|)  (_mm256_abs_epi32).
//  Bajty: t = v*scale; t > 255 ? 255 : (int)(t + 0.5) – dokladnie jak toByte().
//  Brzegi wiersza i koncowki skalarnie.  Wynik identyczny z V0 co do bajtu.
// ============================================================================
#include <algorithm>
#include <cmath>

#include "../kernels.h"
#include "static_masks.h"

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define EDGE_HAVE_AVX2_CODE 1
#define EDGE_TARGET_AVX2 __attribute__((target("avx2")))
#endif

bool cpuHasAvx2()
{
#if defined(EDGE_HAVE_AVX2_CODE)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

#if defined(EDGE_HAVE_AVX2_CODE)
namespace {

enum class Mode { StoreMag, Direct, MaxOnly };

template <class M>
constexpr bool floatSqrtExact()
{
    long px = 0, py = 0;
    for (int v : M::gx.v) if (v > 0) px += v;
    for (int v : M::gy.v) if (v > 0) py += v;
    const long gx = 255 * px, gy = 255 * py;
    return gx * gx + gy * gy < (1L << 24);
}

// --- przebieg poziomy --------------------------------------------------------
template <class M>
inline void hScalar(const std::uint8_t* row, int w, int x, std::int16_t* hs, std::int16_t* hd)
{
    constexpr int K = M::K, R = K / 2;
    int s = 0, d = 0;
    for (int j = 0; j < K; ++j) {
        const int p = row[std::clamp(x + j - R, 0, w - 1)];
        s += M::smooth[j] * p;  d += M::diff[j] * p;
    }
    hs[x] = static_cast<std::int16_t>(s);  hd[x] = static_cast<std::int16_t>(d);
}

template <class M>
EDGE_TARGET_AVX2 void hpass(const std::uint8_t* row, int w, std::int16_t* hs, std::int16_t* hd)
{
    constexpr int K = M::K, R = K / 2;
    int x = 0;
    for (; x < std::min(R, w); ++x) hScalar<M>(row, w, x, hs, hd);
    for (; x + 16 + R <= w; x += 16) {
        __m256i s = _mm256_setzero_si256(), d = _mm256_setzero_si256();
        for (int j = 0; j < K; ++j) {
            const __m256i p = _mm256_cvtepu8_epi16(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x + j - R)));
            if (M::smooth[j] != 0) s = _mm256_add_epi16(s, _mm256_mullo_epi16(p, _mm256_set1_epi16(static_cast<short>(M::smooth[j]))));
            if (M::diff[j]   != 0) d = _mm256_add_epi16(d, _mm256_mullo_epi16(p, _mm256_set1_epi16(static_cast<short>(M::diff[j]))));
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(hs + x), s);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(hd + x), d);
    }
    for (; x < w; ++x) hScalar<M>(row, w, x, hs, hd);
}

// --- modul i zapis ---------------------------------------------------------------
template <class M>
inline void gradScalar(const std::int16_t* const* hs, const std::int16_t* const* hd, int x, int& sx, int& sy)
{
    sx = 0;  sy = 0;
    for (int i = 0; i < M::K; ++i) {
        sx += M::smooth[i] * hd[i][x];
        sy += M::diff[i]   * hs[i][x];
    }
}

template <class M, Norm N>
EDGE_TARGET_AVX2 inline __m256 magnitude8(__m256i sx, __m256i sy)
{
    if constexpr (N == Norm::L1) {
        return _mm256_cvtepi32_ps(_mm256_add_epi32(_mm256_abs_epi32(sx), _mm256_abs_epi32(sy)));
    } else if constexpr (N == Norm::Linf) {
        return _mm256_cvtepi32_ps(_mm256_max_epi32(_mm256_abs_epi32(sx), _mm256_abs_epi32(sy)));
    } else if constexpr (floatSqrtExact<M>()) {
        const __m256 fx = _mm256_cvtepi32_ps(sx), fy = _mm256_cvtepi32_ps(sy);
        return _mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(fx, fx), _mm256_mul_ps(fy, fy)));
    } else {
        const __m256d xl = _mm256_cvtepi32_pd(_mm256_castsi256_si128(sx));
        const __m256d xh = _mm256_cvtepi32_pd(_mm256_extracti128_si256(sx, 1));
        const __m256d yl = _mm256_cvtepi32_pd(_mm256_castsi256_si128(sy));
        const __m256d yh = _mm256_cvtepi32_pd(_mm256_extracti128_si256(sy, 1));
        const __m128 lo = _mm256_cvtpd_ps(_mm256_sqrt_pd(_mm256_add_pd(_mm256_mul_pd(xl, xl), _mm256_mul_pd(yl, yl))));
        const __m128 hi = _mm256_cvtpd_ps(_mm256_sqrt_pd(_mm256_add_pd(_mm256_mul_pd(xh, xh), _mm256_mul_pd(yh, yh))));
        return _mm256_set_m128(hi, lo);
    }
}

template <class M, Norm N, Mode MD>
EDGE_TARGET_AVX2 float vpass(const std::int16_t* const* hs, const std::int16_t* const* hd, int w,
                             float* mag, std::uint8_t* out, float scale, float localMax)
{
    constexpr int K = M::K;
    __m256 vmax = _mm256_set1_ps(localMax);
    const __m256 vscale = _mm256_set1_ps(scale), v255 = _mm256_set1_ps(255.0f), vhalf = _mm256_set1_ps(0.5f);
    int x = 0;
    for (; x + 8 <= w; x += 8) {
        __m256i sx = _mm256_setzero_si256(), sy = _mm256_setzero_si256();
        for (int i = 0; i < K; ++i) {
            if (M::smooth[i] != 0) {
                const __m256i v = _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(hd[i] + x)));
                sx = _mm256_add_epi32(sx, _mm256_mullo_epi32(v, _mm256_set1_epi32(M::smooth[i])));
            }
            if (M::diff[i] != 0) {
                const __m256i v = _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(hs[i] + x)));
                sy = _mm256_add_epi32(sy, _mm256_mullo_epi32(v, _mm256_set1_epi32(M::diff[i])));
            }
        }
        const __m256 m = magnitude8<M, N>(sx, sy);
        if constexpr (MD == Mode::StoreMag) {
            _mm256_storeu_ps(mag + x, m);
            vmax = _mm256_max_ps(vmax, m);
        } else if constexpr (MD == Mode::MaxOnly) {
            vmax = _mm256_max_ps(vmax, m);
        } else {
            const __m256 t = _mm256_mul_ps(m, vscale);
            const __m256i r = _mm256_cvttps_epi32(_mm256_add_ps(t, vhalf));
            const __m256 big = _mm256_cmp_ps(t, v255, _CMP_GT_OQ);
            const __m256i b = _mm256_blendv_epi8(r, _mm256_set1_epi32(255), _mm256_castps_si256(big));
            // 8 x int32 -> 8 x uint8
            const __m128i lo = _mm256_castsi256_si128(b), hi = _mm256_extracti128_si256(b, 1);
            const __m128i w16 = _mm_packus_epi32(lo, hi);
            const __m128i w8 = _mm_packus_epi16(w16, w16);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(out + x), w8);
        }
    }
    // redukcja maksimum z rejestru
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, vmax);
    for (float l : lanes) localMax = std::max(localMax, l);

    for (; x < w; ++x) {
        int sx, sy;
        gradScalar<M>(hs, hd, x, sx, sy);
        const float v = gradMagnitude(sx, sy, N);
        if constexpr (MD == Mode::StoreMag) { mag[x] = v; localMax = std::max(localMax, v); }
        else if constexpr (MD == Mode::MaxOnly) { localMax = std::max(localMax, v); }
        else { out[x] = toByte(v, scale); }
    }
    return localMax;
}

template <class M, Norm N, Mode MD>
float rows(const KernelArgs& a, int y0, int y1, Scratch& s)
{
    constexpr int K = M::K, R = K / 2;
    const int w = a.img.width, h = a.img.height;
    const size_t stride = static_cast<size_t>(w) + 16;   // zapas na odczyt 128-bit
    const std::uint8_t* src = a.img.data.data();

    const size_t need = static_cast<size_t>(2 * K) * stride;
    if (s.i16.size() < need) s.i16.resize(need);
    s.owner.assign(K, -1);
    std::int16_t* ring = s.i16.data();

    const std::int16_t* hs[K];
    const std::int16_t* hd[K];
    float localMax = 0.0f;

    for (int y = y0; y < y1; ++y) {
        for (int i = 0; i < K; ++i) {
            const int sr = std::clamp(y + i - R, 0, h - 1);
            const int slot = sr % K;
            std::int16_t* phs = ring + static_cast<size_t>(slot) * stride;
            std::int16_t* phd = ring + static_cast<size_t>(slot + K) * stride;
            if (s.owner[slot] != sr) {
                hpass<M>(src + static_cast<size_t>(sr) * w, w, phs, phd);
                s.owner[slot] = sr;
            }
            hs[i] = phs;  hd[i] = phd;
        }
        const size_t base = static_cast<size_t>(y) * w;
        localMax = vpass<M, N, MD>(hs, hd, w, a.mag ? a.mag + base : nullptr,
                                   a.out ? a.out + base : nullptr, a.scale, localMax);
    }
    return localMax;
}

template <class M, Norm N>
float rowsMode(const KernelArgs& a, int y0, int y1, Scratch& s)
{
    if (a.mag) return rows<M, N, Mode::StoreMag>(a, y0, y1, s);
    if (a.out) return rows<M, N, Mode::Direct>(a, y0, y1, s);
    return rows<M, N, Mode::MaxOnly>(a, y0, y1, s);
}

template <class M>
float rowsNorm(const KernelArgs& a, int y0, int y1, Scratch& s)
{
    switch (a.norm) {
    case Norm::L1:   return rowsMode<M, Norm::L1>(a, y0, y1, s);
    case Norm::Linf: return rowsMode<M, Norm::Linf>(a, y0, y1, s);
    default:         return rowsMode<M, Norm::L2>(a, y0, y1, s);
    }
}

} // namespace
#endif

float rowsV6(const KernelArgs& a, int y0, int y1, Scratch& s)
{
#if defined(EDGE_HAVE_AVX2_CODE)
    float result = 0.0f;
    bool handled = false;
    dispatchStaticMask(a.mask, [&](auto m) {
        using M = decltype(m);
        if constexpr (M::separable) {
            result = rowsNorm<M>(a, y0, y1, s);
            handled = true;
        }
    });
    if (handled) return result;
#endif
    // Maska nierozdzielna (Roberts) albo brak AVX2 w kompilatorze.
    // Tryb "tylko maksimum" (V8) nie jest obslugiwany przez V2 – liczymy do bufora tymczasowego.
    if (!a.mag && !a.out) {
        AlignedVec<float> tmp(static_cast<size_t>(a.img.width) * a.img.height);
        KernelArgs b{ a.img, a.mask, a.norm, tmp.data(), nullptr, 0.0f };
        return rowsV2(b, y0, y1, s);
    }
    return rowsV2(a, y0, y1, s);
}
