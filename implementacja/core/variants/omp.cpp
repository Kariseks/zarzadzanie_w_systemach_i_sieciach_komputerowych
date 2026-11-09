// ============================================================================
//  OMP – to samo zadanie co wariant T, ale podzial pracy robi OpenMP
//
//  Petla po blokach wierszy z  #pragma omp parallel for schedule(runtime)
//  i redukcja reduction(max:...).  Mapowanie harmonogramow, zeby porownanie
//  z wlasnym RowScheduler bylo uczciwe (te same bloki, to samo jadro):
//    static  -> T rownych blokow, schedule(static)
//    cyclic  -> bloki po 'chunk' wierszy, schedule(static, 1)  (round-robin)
//    dynamic -> bloki po 'chunk' wierszy, schedule(dynamic, 1)
//    guided  -> bloki po 'chunk' wierszy, schedule(guided, 1)
//  Watki OpenMP sa trzymane przez runtime (libgomp) miedzy wywolaniami –
//  odpowiednik naszej puli.  Plik kompilowany z -fopenmp, gdy CMake znajdzie
//  OpenMP; inaczej wariant jest niedostepny.
// ============================================================================
#include <algorithm>

#include "../variants.h"

#if defined(_OPENMP)
#include <omp.h>
#endif

bool openmpAvailable()
{
#if defined(_OPENMP)
    return true;
#else
    return false;
#endif
}

void runOpenMP(const KernelInfo& k, const GrayImage& img, const Mask& mask, const RunConfig& cfg,
               std::vector<Scratch>& scratchPool, AlignedVec<float>& magPool, RunResult& r)
{
#if defined(_OPENMP)
    const int w = img.width, h = img.height;
    const size_t n = static_cast<size_t>(w) * h;
    const int T = cfg.threads > 0 ? cfg.threads : omp_get_num_procs();
    const int chunk = std::max(1, cfg.chunkRows);

    int nBlocks = 0, blockRows = 0;
    switch (cfg.schedule) {
    case Schedule::Static:  omp_set_schedule(omp_sched_static, 0);  nBlocks = T;  break;
    case Schedule::Cyclic:  omp_set_schedule(omp_sched_static, 1);  break;
    case Schedule::Dynamic: omp_set_schedule(omp_sched_dynamic, 1); break;
    case Schedule::Guided:  omp_set_schedule(omp_sched_guided, 1);  break;
    }
    if (cfg.schedule == Schedule::Static) {
        blockRows = 0;                                    // podzial jak w RowScheduler::nextStatic
    } else {
        blockRows = chunk;
        nBlocks = (h + chunk - 1) / chunk;
    }
    auto blockRange = [&](int b, int& y0, int& y1) {
        if (blockRows == 0) {
            const int base = h / T, extra = h % T;
            y0 = b * base + std::min(b, extra);
            y1 = y0 + base + (b < extra ? 1 : 0);
        } else {
            y0 = b * blockRows;
            y1 = std::min(h, y0 + blockRows);
        }
    };

    std::vector<Scratch>  fresh;
    std::vector<Scratch>& scratch = k.reuseBuffers ? scratchPool : fresh;
    if (scratch.size() < static_cast<size_t>(T)) scratch.resize(static_cast<size_t>(T));

    r.threadsUsed = T;          // bufor r.output przygotowal VariantRunner
    std::uint8_t* out = r.output.data();
    Stopwatch sw;

    if (cfg.scaling != Scaling::GlobalMax) {
        const KernelArgs a{ img, mask, cfg.norm, nullptr, out, directScale(mask, cfg.norm, cfg.scaling) };
        #pragma omp parallel for num_threads(T) schedule(runtime)
        for (int b = 0; b < nBlocks; ++b) {
            int y0, y1;
            blockRange(b, y0, y1);
            if (y1 > y0) k.fn(a, y0, y1, scratch[static_cast<size_t>(omp_get_thread_num())]);
        }
        r.stagesMs["conv"] = sw.wallMs();
        return;
    }

    float maxV = 0.0f;
    if (k.recompute) {
        const KernelArgs a1{ img, mask, cfg.norm, nullptr, nullptr, 0.0f };
        #pragma omp parallel for num_threads(T) schedule(runtime) reduction(max:maxV)
        for (int b = 0; b < nBlocks; ++b) {
            int y0, y1;
            blockRange(b, y0, y1);
            if (y1 > y0) maxV = std::max(maxV, k.fn(a1, y0, y1, scratch[static_cast<size_t>(omp_get_thread_num())]));
        }
        r.stagesMs["conv"] = sw.wallMs();
        sw.restart();
        const KernelArgs a2{ img, mask, cfg.norm, nullptr, out, globalScale(maxV) };
        #pragma omp parallel for num_threads(T) schedule(runtime)
        for (int b = 0; b < nBlocks; ++b) {
            int y0, y1;
            blockRange(b, y0, y1);
            if (y1 > y0) k.fn(a2, y0, y1, scratch[static_cast<size_t>(omp_get_thread_num())]);
        }
        r.stagesMs["norm"] = sw.wallMs();
        r.maxGradient = maxV;
        return;
    }

    AlignedVec<float>  freshMag;
    AlignedVec<float>& mag = k.reuseBuffers ? magPool : freshMag;
    if (mag.size() != n) mag.resize(n);
    float* magp = mag.data();

    const KernelArgs a{ img, mask, cfg.norm, magp, nullptr, 0.0f };
    #pragma omp parallel for num_threads(T) schedule(runtime) reduction(max:maxV)
    for (int b = 0; b < nBlocks; ++b) {
        int y0, y1;
        blockRange(b, y0, y1);
        if (y1 > y0) maxV = std::max(maxV, k.fn(a, y0, y1, scratch[static_cast<size_t>(omp_get_thread_num())]));
    }
    r.stagesMs["conv"] = sw.wallMs();

    sw.restart();
    const float scale = globalScale(maxV);
    #pragma omp parallel for num_threads(T) schedule(runtime)
    for (int b = 0; b < nBlocks; ++b) {
        int y0, y1;
        blockRange(b, y0, y1);
        const size_t e = static_cast<size_t>(y1) * w;
        const size_t b = static_cast<size_t>(y0) * w;
        normalizeRange(k, magp + b, out + b, e - b, scale);
    }
    r.stagesMs["norm"] = sw.wallMs();
    r.maxGradient = maxV;
#else
    (void)k; (void)img; (void)mask; (void)cfg; (void)scratchPool; (void)magPool; (void)r;
#endif
}
