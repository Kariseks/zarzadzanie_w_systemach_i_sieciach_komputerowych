// ============================================================================
//  V3 – splot rozdzielny (dwa sploty 1D), bufory int32
//  V4 – jak V3, ale bufory pierscieniowe int16 (polowa ruchu w pamieci),
//       wyrownane do 64 B i trzymane miedzy wywolaniami (KernelInfo::reuseBuffers:
//       bufor modulu i pamiec robocza nie sa alokowane przy kazdym wywolaniu,
//       wiec pomiar nie placi za page faulty "pierwszego dotyku").
//
//  Maska nierozdzielna (Roberts) – obie wersje licza jak V2.
// ============================================================================
#include "../kernels.h"
#include "separable.h"
#include "static_masks.h"

namespace {

template <class Acc>
AlignedVec<Acc>& ringOf(Scratch& s);
template <> AlignedVec<std::int32_t>& ringOf<std::int32_t>(Scratch& s) { return s.i32; }
template <> AlignedVec<std::int16_t>& ringOf<std::int16_t>(Scratch& s) { return s.i16; }

template <class Acc>
float rowsSeparable(const KernelArgs& a, int y0, int y1, Scratch& s)
{
    float result = 0.0f;
    bool handled = false;
    dispatchStaticMask(a.mask, [&](auto m) {
        using M = decltype(m);
        if constexpr (M::separable) {
            const size_t need = static_cast<size_t>(2 * M::K) * static_cast<size_t>(a.img.width);
            auto& ring = ringOf<Acc>(s);
            if (ring.size() < need) ring.resize(need);
            s.owner.assign(M::K, -1);
            result = a.mag ? separableRows<M, Acc, true>(a, y0, y1, ring.data(), s.owner)
                           : separableRows<M, Acc, false>(a, y0, y1, ring.data(), s.owner);
            handled = true;
        }
    });
    return handled ? result : rowsV2(a, y0, y1, s);
}

} // namespace

float rowsV3(const KernelArgs& a, int y0, int y1, Scratch& s) { return rowsSeparable<std::int32_t>(a, y0, y1, s); }
float rowsV4(const KernelArgs& a, int y0, int y1, Scratch& s) { return rowsSeparable<std::int16_t>(a, y0, y1, s); }
