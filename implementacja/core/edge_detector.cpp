#include "edge_detector.h"

#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>

namespace {
std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

const char* toString(ThreadMech m) { return m == ThreadMech::Spawn ? "spawn" : "pool"; }

const char* toString(Reduce r)
{
    switch (r) {
    case Reduce::Local:         return "local";
    case Reduce::Atomic:        return "atomic";
    case Reduce::Mutex:         return "mutex";
    case Reduce::PixelPadded:   return "px_pad";
    case Reduce::PixelUnpadded: return "px_nopad";
    }
    return "?";
}

bool parseThreadMech(const std::string& s, ThreadMech& out)
{
    const std::string v = lower(s);
    if (v == "pool")  { out = ThreadMech::Pool;  return true; }
    if (v == "spawn") { out = ThreadMech::Spawn; return true; }
    return false;
}

bool parseReduce(const std::string& s, Reduce& out)
{
    const std::string v = lower(s);
    if (v == "local")    { out = Reduce::Local;         return true; }
    if (v == "atomic")   { out = Reduce::Atomic;        return true; }
    if (v == "mutex")    { out = Reduce::Mutex;         return true; }
    if (v == "px_pad")   { out = Reduce::PixelPadded;   return true; }
    if (v == "px_nopad") { out = Reduce::PixelUnpadded; return true; }
    return false;
}

EdgeDetector::Config EdgeDetector::normalize(Config c)
{
    if (c.threads <= 0) {
        c.threads = static_cast<int>(std::thread::hardware_concurrency());
        if (c.threads <= 0) c.threads = 1;
    }
    if (c.chunkRows < 1) c.chunkRows = 1;
    return c;
}

EdgeDetector::EdgeDetector(Config cfg)
    : cfg_(normalize(cfg)),
      cpus_(cpuOrder(cfg_.affinity))
{
    if (cfg_.mech == ThreadMech::Pool)
        pool_ = std::make_unique<ThreadPool>(cfg_.threads, cpus_);
}

template <class RowFn>
PhaseStats EdgeDetector::runPhase(int totalRows, RowFn&& rowFn)
{
    const int T = cfg_.threads;
    RowScheduler sched(totalRows, T, cfg_.schedule, cfg_.chunkRows);

    PhaseStats ps;
    ps.perThread.resize(static_cast<size_t>(T));

    Stopwatch sw;
    const auto phaseStart = std::chrono::steady_clock::now();

    const std::function<void(int)> job = [&](int tid) {
        const double cpu0 = threadCpuTimeSec();
        ThreadStats  st;

        int y0 = 0, y1 = 0;
        while (sched.next(tid, y0, y1)) {
            rowFn(tid, y0, y1);
            ++st.chunks;
            st.rows += y1 - y0;
        }

        st.cpuMs  = (threadCpuTimeSec() - cpu0) * 1000.0;
        st.busyMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - phaseStart).count();
        ps.perThread[static_cast<size_t>(tid)] = st;   // kazdy watek pisze swoj element
    };
    if (pool_) pool_->runOnAll(job);
    else       ThreadPool::runOnAllSpawn(job, T, cpus_);

    ps.wallMs = sw.wallMs();
    ps.cpuMs  = sw.cpuMs();
    return ps;
}

void EdgeDetector::process(const GrayImage& img, const Mask& mask, Norm norm, Scaling scaling,
                           const KernelInfo& kernel, Reduce reduce, std::vector<std::uint8_t>& out)
{
    const int    w = img.width;
    const int    h = img.height;
    const size_t n = static_cast<size_t>(w) * h;
    const auto   T = static_cast<size_t>(cfg_.threads);

    // Pamiec robocza: trzymana miedzy wywolaniami (V4+) albo swieza (V0..V3)
    std::vector<Scratch> freshScratch;
    std::vector<Scratch>* scratch = &scratch_;
    if (!kernel.reuseBuffers) { freshScratch.resize(T); scratch = &freshScratch; }
    else if (scratch_.size() != T) scratch_.assign(T, Scratch{});

    report_ = DetectorReport{};

    if (scaling != Scaling::GlobalMax) {
        // --- Jeden etap: wynik od razu w bajtach, bez bariery i redukcji -------
        const KernelArgs args{ img, mask, norm, nullptr, out.data(), directScale(mask, norm, scaling) };
        report_.convolution = runPhase(h, [&](int tid, int y0, int y1) {
            kernel.fn(args, y0, y1, (*scratch)[static_cast<size_t>(tid)]);
        });
        return;
    }

    // Lokalne maksimum kazdego watku – osobna linia cache na watek
    struct alignas(64) PaddedFloat { float v = 0.0f; };
    std::vector<PaddedFloat> localMax(T);

    if (kernel.recompute) {
        // --- V8: etap 1 tylko maksimum, etap 2 od nowa z zapisem bajtow --------
        const KernelArgs a1{ img, mask, norm, nullptr, nullptr, 0.0f };
        report_.convolution = runPhase(h, [&](int tid, int y0, int y1) {
            const float m = kernel.fn(a1, y0, y1, (*scratch)[static_cast<size_t>(tid)]);
            if (m > localMax[static_cast<size_t>(tid)].v) localMax[static_cast<size_t>(tid)].v = m;
        });
        float maxV = 0.0f;
        for (const auto& lm : localMax) maxV = std::max(maxV, lm.v);
        report_.maxGradient = maxV;
        const KernelArgs a2{ img, mask, norm, nullptr, out.data(), globalScale(maxV) };
        report_.normalization = runPhase(h, [&](int tid, int y0, int y1) {
            kernel.fn(a2, y0, y1, (*scratch)[static_cast<size_t>(tid)]);
        });
        return;
    }

    AlignedVec<float>  freshMag;
    AlignedVec<float>* mag = &mag_;
    if (!kernel.reuseBuffers) { freshMag.resize(n); mag = &freshMag; }
    else if (mag_.size() != n) mag_.resize(n);
    float* magp = mag->data();

    // Wspolne zasoby dla trybow z wspolzawodnictwem
    std::atomic<float> sharedMax{ 0.0f };
    std::mutex         sharedMutex;
    float              sharedPlain = 0.0f;
    std::vector<float> unpadded(T, 0.0f);   // komorki obok siebie -> false sharing

    // --- Etap 1: splot maska ------------------------------------------------
    const KernelArgs args{ img, mask, norm, magp, nullptr, 0.0f };
    report_.convolution = runPhase(h, [&](int tid, int y0, int y1) {
        const float m = kernel.fn(args, y0, y1, (*scratch)[static_cast<size_t>(tid)]);
        const size_t b = static_cast<size_t>(y0) * w, e = static_cast<size_t>(y1) * w;
        switch (reduce) {
        case Reduce::Local:
            if (m > localMax[static_cast<size_t>(tid)].v) localMax[static_cast<size_t>(tid)].v = m;
            break;
        case Reduce::Atomic:
            for (size_t i = b; i < e; ++i) {
                const float v = magp[i];
                float cur = sharedMax.load(std::memory_order_relaxed);
                while (v > cur && !sharedMax.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
            }
            break;
        case Reduce::Mutex:
            for (size_t i = b; i < e; ++i) {
                std::lock_guard<std::mutex> lk(sharedMutex);
                if (magp[i] > sharedPlain) sharedPlain = magp[i];
            }
            break;
        case Reduce::PixelPadded: {
            volatile float* cell = &localMax[static_cast<size_t>(tid)].v;
            for (size_t i = b; i < e; ++i) { const float c = *cell; *cell = magp[i] > c ? magp[i] : c; }
            break;
        }
        case Reduce::PixelUnpadded: {
            volatile float* cell = &unpadded[static_cast<size_t>(tid)];
            for (size_t i = b; i < e; ++i) { const float c = *cell; *cell = magp[i] > c ? magp[i] : c; }
            break;
        }
        }
    });

    // --- Redukcja: globalne maksimum (po barierze, jednowatkowo) -------------
    float maxV = 0.0f;
    switch (reduce) {
    case Reduce::Local:
    case Reduce::PixelPadded:   for (const auto& lm : localMax) maxV = std::max(maxV, lm.v); break;
    case Reduce::PixelUnpadded: for (float v : unpadded) maxV = std::max(maxV, v); break;
    case Reduce::Atomic:        maxV = sharedMax.load(); break;
    case Reduce::Mutex:         maxV = sharedPlain; break;
    }
    report_.maxGradient = maxV;

    // --- Etap 2: normalizacja do 0..255 --------------------------------------
    const float scale = globalScale(maxV);
    std::uint8_t* outp = out.data();
    report_.normalization = runPhase(h, [&](int, int y0, int y1) {
        const size_t b = static_cast<size_t>(y0) * w, e = static_cast<size_t>(y1) * w;
        normalizeRange(kernel, magp + b, outp + b, e - b, scale);
    });

}
