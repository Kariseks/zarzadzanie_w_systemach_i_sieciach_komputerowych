#include "edge_detector.h"

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
      pool_(cfg_.threads)
{}

template <class RowFn>
PhaseStats EdgeDetector::runPhase(int totalRows, RowFn&& rowFn)
{
    const int T = cfg_.threads;
    RowScheduler sched(totalRows, T, cfg_.schedule, cfg_.chunkRows);

    PhaseStats ps;
    ps.perThread.resize(static_cast<size_t>(T));

    Stopwatch sw;
    const auto phaseStart = std::chrono::steady_clock::now();

    pool_.runOnAll([&](int tid) {
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
    });

    ps.wallMs = sw.wallMs();
    ps.cpuMs  = sw.cpuMs();
    return ps;
}

std::vector<std::uint8_t> EdgeDetector::process(const GrayImage& img, const Mask& mask)
{
    const int    w = img.width;
    const int    h = img.height;
    const size_t n = static_cast<size_t>(w) * h;

    std::vector<float>        mag(n);
    std::vector<std::uint8_t> out(n);

    // Lokalne maksimum kazdego watku – osobna linia cache na watek
    struct alignas(64) PaddedFloat { float v = 0.0f; };
    std::vector<PaddedFloat> localMax(static_cast<size_t>(cfg_.threads));

    // --- Etap 1: splot maska ------------------------------------------------
    report_.convolution = runPhase(h, [&](int tid, int y0, int y1) {
        const float m = edgeRows(img, mag, mask, y0, y1);
        if (m > localMax[static_cast<size_t>(tid)].v) localMax[static_cast<size_t>(tid)].v = m;
    });

    // --- Redukcja: globalne maksimum (po barierze, jednowatkowo) -------------
    float maxV = 0.0f;
    for (const auto& lm : localMax) maxV = std::max(maxV, lm.v);
    report_.maxGradient = maxV;

    // --- Etap 2: normalizacja do 0..255 --------------------------------------
    report_.normalization = runPhase(h, [&](int, int y0, int y1) {
        normalizeRows(mag, out, w, maxV, y0, y1);
    });

    return out;
}
