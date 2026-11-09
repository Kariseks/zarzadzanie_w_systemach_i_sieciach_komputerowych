#include "variants.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "gpu/gpu_opencl.h"
#include "measure.h"

namespace {

constexpr const char* kNoAvx2  = "procesor nie obsluguje AVX2";
const bool            kAvx2    = cpuHasAvx2();
const bool            kOmp     = openmpAvailable();
const bool            kGpu     = gpuCompiledIn();
const bool            kAi      = aiVariantImplemented();
constexpr const char* kNoGpu   = "program zbudowany bez OpenCL (sudo apt install ocl-icd-opencl-dev opencl-headers)";

const std::vector<VariantInfo> kVariants = {
    { VariantId::V0_Naive,     "V0",  "Naiwny: clamp w petli, maska z wektora, bufory alokowane przy kazdym wywolaniu", false, true,  "", 0, { rowsV0, false } },
    { VariantId::V1_Borders,   "V1",  "V0 + brzegi obrazu obsluzone poza glowna petla",                                false, true,  "", 0, { rowsV1, false } },
    { VariantId::V2_ConstMask, "V2",  "V1 + maska jako stala czasu kompilacji (szablon)",                              false, true,  "", 0, { rowsV2, false } },
    { VariantId::V3_Separable, "V3",  "V2 + splot rozdzielny (dwa sploty 1D, bufor K wierszy int32)",                  false, true,  "", 0, { rowsV3, false } },
    { VariantId::V4_Types,     "V4",  "V3 + bufory int16 wyrownane do 64 B, trzymane miedzy wywolaniami",              false, true,  "", 0, { rowsV4, true  } },
    { VariantId::V5_AutoVec,   "V5",  "V4 przepisany pod auto-wektoryzacje (AVX2, __restrict, proste petle)",          false, kAvx2, kNoAvx2, 0, { rowsV5, true } },
    { VariantId::V6_Avx2,      "V6",  "V4 z recznym SIMD AVX2 (16 pikseli poziomo, 8 pionowo)",                        false, kAvx2, kNoAvx2, 0, { rowsV6, true } },
    { VariantId::V8_Tiled,     "V8",  "V6 + przeliczanie zamiast bufora modulu (GlobalMax: 2 x obliczenia, 0 x 4 B/piksel)", false, kAvx2, kNoAvx2, 0, { rowsV6, true, true } },
    { VariantId::Threads,      "T",   "Wielowatkowo: pula watkow + scheduler, jadro wybierane parametrem kernel",      true,  true,  "", 0, {} },
    { VariantId::AI,           "AI",  "Wersja zaproponowana przez model AI (implementacja/ai/PROMPT.md)",              false, kAi,   "kod od AI nie zostal jeszcze wklejony (implementacja/ai/PROMPT.md)", 0, { rowsAI, false } },
    { VariantId::OpenMP,       "OMP", "OpenMP parallel for schedule(runtime), jadro wybierane parametrem kernel",      true,  kOmp,  "program zbudowany bez OpenMP", 0, {} },
    { VariantId::GPU_OpenCL,   "GPU", "OpenCL na GPU: kernel naiwny, kazdy piksel czyta K x K z pamieci globalnej",    false, kGpu,  kNoGpu, 1, {} },
    { VariantId::GPU_OpenCL_Static, "GPUC", "GPU + maska wkompilowana w kernel (petle rozwijane, jak V2)",            false, kGpu,  kNoGpu, 1, {} },
    { VariantId::GPU_OpenCL_Local, "GPUL", "GPUC + kafel 16x16 z obrzezem w pamieci lokalnej (__local)",              false, kGpu,  kNoGpu, 1, {} },

    // --- 5 wersji ze sprawozdania: kazda = W1 + jeden rodzaj przyspieszenia, porownywane z W1 ---
    { VariantId::W1, "W1", "Kod bazowy: sekwencyjny, 1 rdzen (= V0)",                                                   false, true,  "", 0, { rowsV0, false } },
    { VariantId::W2, "W2", "W1 + optymalizacja algorytmu i pamieci: brzegi, stala maska, splot rozdzielny, int16 (= V4)", false, true,  "", 0, { rowsV4, true } },
    { VariantId::W3, "W3", "W1 + wektoryzacja SIMD AVX2 (8 pikseli na instrukcje), ten sam algorytm",                   false, kAvx2, kNoAvx2, 0, { rowsW3, false, false, normalizeW3 } },
    { VariantId::W4, "W4", "W1 + wiele rdzeni CPU: pula watkow + scheduler (= T z jadrem V0)",                          true,  true,  "", 0, {} },
    { VariantId::W5, "W5", "W1 + GPU: kernel OpenCL, 1 watek sprzetowy na piksel (= GPU)",                              false, kGpu,  kNoGpu, 1, {} },
};

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

} // namespace

const std::vector<VariantInfo>& allVariants() { return kVariants; }

const VariantInfo& variantInfo(VariantId id)
{
    for (const auto& v : kVariants)
        if (v.id == id) return v;
    throw std::logic_error("variantInfo: brak wpisu dla wariantu");
}

const VariantInfo* findVariant(const std::string& name)
{
    const std::string n = upper(name);
    for (const auto& v : kVariants)
        if (n == v.name) return &v;
    return nullptr;
}

bool isKernelVariant(const VariantInfo& v)
{
    return v.available && !v.multithreaded && v.kernel.fn != nullptr;
}

// ----------------------------------------------------------------------------

VariantRunner::VariantRunner()  = default;
VariantRunner::~VariantRunner() = default;

EdgeDetector& VariantRunner::detectorFor(const RunConfig& cfg)
{
    // Pula watkow tworzona tylko przy zmianie konfiguracji, wiec kolejne
    // powtorzenia nie placa za tworzenie watkow.
    const EdgeDetectorConfig want = EdgeDetector::normalize({ cfg.threads, cfg.schedule, cfg.chunkRows,
                                                              cfg.mech, cfg.affinity });
    if (!detector_ ||
        detector_->threadCount()     != want.threads ||
        detector_->schedule()        != want.schedule ||
        detector_->chunkRows()       != want.chunkRows ||
        detector_->config().mech     != want.mech ||
        detector_->config().affinity != want.affinity) {
        detector_.reset();                           // najpierw zwolnij stare watki
        detector_ = std::make_unique<EdgeDetector>(want);
    }
    return *detector_;
}

void VariantRunner::runSingle(const VariantInfo& v, const GrayImage& img, const Mask& mask,
                              const RunConfig& cfg, RunResult& r)
{
    const int    w = img.width;
    const int    h = img.height;
    const size_t n = static_cast<size_t>(w) * h;
    const KernelInfo& k = v.kernel;

    r.threadsUsed = 1;

    Scratch  fresh;
    Scratch& scratch = k.reuseBuffers ? scratch_ : fresh;

    Stopwatch sw;
    if (cfg.scaling != Scaling::GlobalMax) {
        const KernelArgs a{ img, mask, cfg.norm, nullptr, r.output.data(),
                            directScale(mask, cfg.norm, cfg.scaling) };
        k.fn(a, 0, h, scratch);
        r.stagesMs["conv"] = sw.wallMs();
        return;
    }

    if (k.recompute) {
        // V8: przebieg 1 – tylko maksimum, przebieg 2 – od nowa, od razu bajty
        const KernelArgs a1{ img, mask, cfg.norm, nullptr, nullptr, 0.0f };
        const float maxV = k.fn(a1, 0, h, scratch);
        r.stagesMs["conv"] = sw.wallMs();
        sw.restart();
        const KernelArgs a2{ img, mask, cfg.norm, nullptr, r.output.data(), globalScale(maxV) };
        k.fn(a2, 0, h, scratch);
        r.stagesMs["norm"] = sw.wallMs();
        r.maxGradient = maxV;
        return;
    }

    AlignedVec<float>  freshMag;
    AlignedVec<float>& mag = k.reuseBuffers ? mag_ : freshMag;
    if (mag.size() != n) mag.resize(n);

    const KernelArgs a{ img, mask, cfg.norm, mag.data(), nullptr, 0.0f };
    const float maxV = k.fn(a, 0, h, scratch);
    r.stagesMs["conv"] = sw.wallMs();

    sw.restart();
    const float scale = globalScale(maxV);
    std::uint8_t* out = r.output.data();
    const float*  m   = mag.data();
    normalizeRange(k, m, out, n, scale);
    r.stagesMs["norm"] = sw.wallMs();
    r.maxGradient = maxV;
}

RunResult VariantRunner::run(VariantId id, const GrayImage& img, const Mask& mask,
                             const RunConfig& cfg)
{
    // Aliasy W4 i W5: ten sam kod co T z jadrem V0 i naiwny kernel GPU
    if (id == VariantId::W4 && variantInfo(id).available) {
        RunConfig c = cfg;
        c.kernel = VariantId::V0_Naive;
        return run(VariantId::Threads, img, mask, c);
    }
    if (id == VariantId::W5 && variantInfo(id).available) return run(VariantId::GPU_OpenCL, img, mask, cfg);

    const VariantInfo& info = variantInfo(id);
    if (!info.available)
        throw std::runtime_error(std::string("wariant ") + info.name +
                                 " niedostepny: " + info.unavailableReason);

    // Przygotowanie zasobow (pula watkow) POZA pomiarem
    EdgeDetector* det = nullptr;
    const VariantInfo* kernel = nullptr;
    if (id == VariantId::Threads || id == VariantId::OpenMP) {
        kernel = &variantInfo(cfg.kernel);
        if (!isKernelVariant(*kernel))
            throw std::runtime_error(std::string("jadro ") + kernel->name +
                                     " nie moze byc uzyte w wariancie " + info.name);
        if (id == VariantId::Threads) det = &detectorFor(cfg);
    }
    const bool gpu = isGpuVariant(id);
    if (gpu) gpuPrepare(img, mask);   // kontekst, kompilacja kernela, bufory – poza pomiarem

    RunResult r;
    Stopwatch sw;

    // Bufor wyniku: warianty naiwne (V0..V3 i T/OMP z takim jadrem) alokuja i zeruja go
    // przy kazdym wywolaniu – to czesc ich kosztu (jednowatkowa, wiec ogranicza T zgodnie
    // z prawem Amdahla).  Warianty z reuseBuffers (V4+, GPU) uzywaja bufora z poprzedniego
    // wywolania – bez alokacji, zerowania i page faultow.
    const size_t npx = static_cast<size_t>(img.width) * img.height;
    const bool reuseOut = gpu || (kernel ? kernel->kernel.reuseBuffers : info.kernel.reuseBuffers);
    if (reuseOut) {
        r.output.swap(outBuf_);
        if (r.output.size() != npx) r.output.resize(npx);
    } else {
        r.output.assign(npx, 0);
    }

    if (det) {
        det->process(img, mask, cfg.norm, cfg.scaling, kernel->kernel, cfg.reduce, r.output);
        const DetectorReport& rep = det->lastReport();
        r.stagesMs["conv"] = rep.convolution.wallMs;
        if (cfg.scaling == Scaling::GlobalMax) r.stagesMs["norm"] = rep.normalization.wallMs;
        r.threadsUsed = det->threadCount();
        r.perThread   = rep.convolution.perThread;
        r.maxGradient = rep.maxGradient;

        double first = 1e300, last = 0.0;
        for (const auto& t : r.perThread) {
            if (t.rows == 0) continue;               // watek bez pracy nie liczy sie do nierownowagi
            first = std::min(first, t.busyMs);
            last  = std::max(last,  t.busyMs);
        }
        r.imbalanceMs = last > 0.0 ? last - first : 0.0;
    } else if (gpu) {
        runGpu(id == VariantId::GPU_OpenCL ? GpuKernel::Naive
               : id == VariantId::GPU_OpenCL_Static ? GpuKernel::Static : GpuKernel::Local,
               img, mask, cfg, r);
    } else if (id == VariantId::OpenMP) {
        runOpenMP(kernel->kernel, img, mask, cfg, ompScratch_, ompMag_, r);
    } else if (info.kernel.fn) {
        runSingle(info, img, mask, cfg, r);
    } else {
        throw std::logic_error("wariant oznaczony jako dostepny, ale bez implementacji");
    }

    r.wallMs = sw.wallMs();
    r.cpuMs  = sw.cpuMs();
    return r;
}

RunResult runVariant(VariantId id, const GrayImage& img, const Mask& mask, const RunConfig& cfg)
{
    thread_local VariantRunner runner;
    return runner.run(id, img, mask, cfg);
}
