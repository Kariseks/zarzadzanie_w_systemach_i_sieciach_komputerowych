// ============================================================================
//  W3 – wersja W1 (V0) z jedna zmiana: wektoryzacja SIMD (AVX2)
//
//  Algorytm bez zmian wzgledem W1: splot 2D k x k, wspolczynniki maski
//  czytane z std::vector w czasie wykonania (bez pomijania zer, bez
//  rozdzielnosci), modul w tej samej arytmetyce, bufory alokowane przy
//  kazdym wywolaniu.  Zmiana: 8 pikseli wyniku liczonych naraz:
//    * dla kazdego wspolczynnika maski: 8 pikseli uint8 -> int32
//      (_mm256_cvtepu8_epi32), mnozenie przez wspolczynnik
//      (_mm256_mullo_epi32), sumowanie Gx i Gy w rejestrach,
//    * modul: L2 przez _mm256_sqrt_ps (gdy modul^2 < 2^24 – wynik dokladny)
//      albo _mm256_sqrt_pd (double), L1/Linf przez _mm256_abs_epi32,
//    * skalowanie do 0..255 (osobny przebieg) – tez 8 pikseli na instrukcje.
//  Wiersze zrodlowe: indeks wiersza obcinany raz na (y, ky) – w wektorze
//  nie da sie wykonac std::clamp osobno dla kazdego piksela.  Kolumny
//  brzegowe (x < R, x >= w - R) i koncowka wiersza liczone kodem W1
//  (gradientAtRef), wiec wynik jest identyczny z W1 co do bajtu.
// ============================================================================
#include <algorithm>
#include <cmath>

#include "../kernels.h"
#include "../kernels_ref.h"

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define EDGE_W3_AVX2 1
#define EDGE_TARGET_AVX2 __attribute__((target("avx2")))
#endif

#if defined(EDGE_W3_AVX2)
namespace {

template <Norm N, bool FloatExact>
EDGE_TARGET_AVX2 inline __m256 magnitude8(__m256i sx, __m256i sy)
{
    if constexpr (N == Norm::L1) {
        return _mm256_cvtepi32_ps(_mm256_add_epi32(_mm256_abs_epi32(sx), _mm256_abs_epi32(sy)));
    } else if constexpr (N == Norm::Linf) {
        return _mm256_cvtepi32_ps(_mm256_max_epi32(_mm256_abs_epi32(sx), _mm256_abs_epi32(sy)));
    } else if constexpr (FloatExact) {
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

// 8 x float -> 8 x uint8: t = v*scale; t > 255 ? 255 : (int)(t + 0.5)  – jak toByte()
EDGE_TARGET_AVX2 inline void store8Bytes(std::uint8_t* out, __m256 v, __m256 vscale)
{
    const __m256 t = _mm256_mul_ps(v, vscale);
    const __m256i r = _mm256_cvttps_epi32(_mm256_add_ps(t, _mm256_set1_ps(0.5f)));
    const __m256 big = _mm256_cmp_ps(t, _mm256_set1_ps(255.0f), _CMP_GT_OQ);
    const __m256i b = _mm256_blendv_epi8(r, _mm256_set1_epi32(255), _mm256_castps_si256(big));
    const __m128i w16 = _mm_packus_epi32(_mm256_castsi256_si128(b), _mm256_extracti128_si256(b, 1));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(out), _mm_packus_epi16(w16, w16));
}

template <Norm N, bool FloatExact, bool Global>
EDGE_TARGET_AVX2 float rows(const KernelArgs& a, int y0, int y1)
{
    const GrayImage& img = a.img;
    const Mask& m = a.mask;
    const int w = img.width, h = img.height;
    const int k = m.size, r = k / 2;
    const std::uint8_t* src = img.data.data();
    const int xa = std::min(r, w);
    const int xb = std::max(xa, w - r);
    const __m256 vscale = _mm256_set1_ps(a.scale);

    float localMax = 0.0f;
    __m256 vmax = _mm256_setzero_ps();

    auto scalarPixel = [&](int x, int y) {
        int sx, sy;
        gradientAtRef(img, m, x, y, sx, sy);          // dokladnie kod W1
        storePixel<Global>(a, static_cast<size_t>(y) * w + x, sx, sy, localMax);
    };

    for (int y = y0; y < y1; ++y) {
        const size_t base = static_cast<size_t>(y) * w;
        for (int x = 0; x < xa; ++x) scalarPixel(x, y);

        int x = xa;
        for (; x + 8 <= xb; x += 8) {
            __m256i sx = _mm256_setzero_si256(), sy = _mm256_setzero_si256();
            for (int ky = 0; ky < k; ++ky) {
                const std::uint8_t* row = src + static_cast<size_t>(std::clamp(y + ky - r, 0, h - 1)) * w;
                for (int kx = 0; kx < k; ++kx) {
                    const __m256i p = _mm256_cvtepu8_epi32(
                        _mm_loadl_epi64(reinterpret_cast<const __m128i*>(row + x + kx - r)));
                    const int idx = ky * k + kx;
                    sx = _mm256_add_epi32(sx, _mm256_mullo_epi32(p, _mm256_set1_epi32(m.gx[idx])));
                    sy = _mm256_add_epi32(sy, _mm256_mullo_epi32(p, _mm256_set1_epi32(m.gy[idx])));
                }
            }
            const __m256 v = magnitude8<N, FloatExact>(sx, sy);
            if constexpr (Global) {
                _mm256_storeu_ps(a.mag + base + x, v);
                vmax = _mm256_max_ps(vmax, v);
            } else {
                store8Bytes(a.out + base + x, v, vscale);
            }
        }
        for (; x < w; ++x) scalarPixel(x, y);   // koncowka wnetrza i prawy brzeg
    }

    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, vmax);
    for (float l : lanes) localMax = std::max(localMax, l);
    return localMax;
}

template <Norm N>
float rowsNorm(const KernelArgs& a, int y0, int y1, bool floatExact)
{
    if (floatExact) return a.mag ? rows<N, true, true>(a, y0, y1) : rows<N, true, false>(a, y0, y1);
    return a.mag ? rows<N, false, true>(a, y0, y1) : rows<N, false, false>(a, y0, y1);
}

} // namespace
#endif

float rowsW3(const KernelArgs& a, int y0, int y1, Scratch& s)
{
#if defined(EDGE_W3_AVX2)
    if (a.mag || a.out) {
        // Czy modul^2 miesci sie dokladnie w float (wtedy sqrtf == (float)sqrt(double))
        const double t = theoreticalMax(a.mask, Norm::L2);
        const bool floatExact = t * t < 16777216.0;
        switch (a.norm) {
        case Norm::L1:   return rowsNorm<Norm::L1>(a, y0, y1, floatExact);
        case Norm::Linf: return rowsNorm<Norm::Linf>(a, y0, y1, floatExact);
        default:         return rowsNorm<Norm::L2>(a, y0, y1, floatExact);
        }
    }
#endif
    return rowsV0(a, y0, y1, s);   // brak AVX2 w kompilatorze albo tryb "tylko maksimum"
}

void normalizeW3(const float* mag, std::uint8_t* out, size_t n, float scale)
{
#if defined(EDGE_W3_AVX2)
    struct Impl {
        EDGE_TARGET_AVX2 static void run(const float* mag, std::uint8_t* out, size_t n, float scale)
        {
            const __m256 vscale = _mm256_set1_ps(scale);
            size_t i = 0;
            for (; i + 8 <= n; i += 8) store8Bytes(out + i, _mm256_loadu_ps(mag + i), vscale);
            for (; i < n; ++i) out[i] = toByte(mag[i], scale);
        }
    };
    Impl::run(mag, out, n, scale);
#else
    for (size_t i = 0; i < n; ++i) out[i] = toByte(mag[i], scale);
#endif
}
