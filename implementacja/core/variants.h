// ============================================================================
//  variants.h  –  wszystkie wersje kodu wywolywane przez JEDNA funkcje
//
//  CLI, edge_bench, testy poprawnosci i harness pomiarowy znaja tylko
//  runVariant() / VariantRunner::run().  Dzieki temu kazda wersja jest
//  mierzona w tych samych granicach (od wejscia do gotowego obrazu 0..255,
//  bez wczytywania i zapisu pliku).
//
//  Warianty 1-watkowe (V0..V8) to jadra z kernels.h.  Wariant T to
//  wielowatkowa pula + scheduler, ktora uruchamia WYBRANE jadro
//  (RunConfig::kernel, domyslnie V0) – dzieki temu da sie zmierzyc, ile daje
//  samo zrownoleglenie dla kazdej optymalizacji jednowatkowej.
//
//  Dodanie wariantu jednowatkowego:
//    1. funkcja rowsVn w core/variants/<plik>.cpp (deklaracja w kernels.h),
//    2. wpis w tablicy w variants.cpp (available = true, kernel = rowsVn).
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "edge_detector.h"   // ThreadStats, Schedule
#include "filter.h"
#include "image.h"
#include "kernels.h"
#include "masks.h"

enum class VariantId {
    V0_Naive,       // referencja: clamp w petli, maska z wektora
    V1_Borders,     // brzegi poza glowna petla
    V2_ConstMask,   // maska jako stala czasu kompilacji
    V3_Separable,   // splot rozdzielny, bufory int32
    V4_Types,       // jak V3 + bufory int16, wyrownane, trzymane miedzy wywolaniami
    V5_AutoVec,     // petle pod auto-wektoryzacje
    V6_Avx2,        // reczne SIMD AVX2
    V8_Tiled,       // kafelkowanie / jeden przebieg
    Threads,        // wielowatkowo: pula + scheduler, jadro z RunConfig::kernel
    AI,             // wersja zaproponowana przez AI
    OpenMP,         // OpenMP parallel for schedule(...)
    GPU_OpenCL,     // OpenCL na GPU, kernel naiwny (pamiec globalna)
    GPU_OpenCL_Static, // OpenCL na GPU, maska wkompilowana
    GPU_OpenCL_Local, // OpenCL na GPU, maska wkompilowana + kafel w pamieci lokalnej

    // --- 5 wersji opisywanych w sprawozdaniu: kazda = W1 + jeden rodzaj przyspieszenia ---
    W1,             // kod bazowy, sekwencyjny (= V0)
    W2,             // algorytm i pamiec, 1 rdzen (= V4)
    W3,             // W1 + wektoryzacja AVX2 (ten sam algorytm)
    W4,             // W1 + wiele rdzeni CPU (= T z jadrem V0)
    W5,             // W1 + GPU (= GPU, kernel naiwny)
};

// Wariant wielowatkowy z pula (parametry mech / reduce / affinity) – T i W4
inline bool isThreadPoolVariant(VariantId id) { return id == VariantId::Threads || id == VariantId::W4; }
// Wariant liczony na GPU
inline bool isGpuVariant(VariantId id)
{
    return id == VariantId::GPU_OpenCL || id == VariantId::GPU_OpenCL_Static ||
           id == VariantId::GPU_OpenCL_Local || id == VariantId::W5;
}

// Parametry wykonania; warianty 1-watkowe ignoruja pola dotyczace watkow
struct RunConfig {
    int       threads   = 0;                  // 0 = liczba rdzeni logicznych
    Schedule  schedule  = Schedule::Dynamic;
    int       chunkRows = 16;
    Norm      norm      = Norm::L2;
    Scaling   scaling   = Scaling::GlobalMax;
    VariantId kernel    = VariantId::V0_Naive; // jadro dla T / OMP (wariant 1-watkowy)
    // tylko T:
    ThreadMech mech     = ThreadMech::Pool;
    Reduce     reduce   = Reduce::Local;
    Affinity   affinity = Affinity::None;
    // tylko GPU: false = "auto" (dokladnie: float gdy wystarcza, inaczej double), true = zawsze float
    bool       gpuFp32  = false;
    // tylko kernel naiwny GPU (GPU / W5): rozmiar grupy roboczej i liczba pikseli na work-item
    int        gpuWgX   = 16;
    int        gpuWgY   = 16;
    int        gpuPpw   = 1;
};

struct RunResult {
    std::vector<std::uint8_t>     output;            // obraz krawedzi 0..255
    double                        wallMs = 0.0;      // czas zegarowy calego wywolania
    double                        cpuMs  = 0.0;      // czas CPU procesu w tym czasie
    std::map<std::string, double> stagesMs;          // "conv", "norm", (GPU: "upload", ...)
    int                           threadsUsed = 1;
    std::vector<ThreadStats>      perThread;         // tylko warianty wielowatkowe (etap splotu)
    double                        imbalanceMs = 0.0; // koniec najpozniejszego - najwczesniejszego watku
    float                         maxGradient = 0.0f;
    long long                     gpuWorkItems = 0;  // GPU: liczba uruchomionych work-items (watkow logicznych)
};

struct VariantInfo {
    VariantId   id;
    const char* name;          // krotki identyfikator: "V0", "T", "GPU", ...
    const char* description;
    bool        multithreaded;
    bool        available;     // false = brak implementacji / brak sprzetu
    const char* unavailableReason;
    // Dopuszczalna roznica wzgledem V0 (poziomy szarosci):
    //   0  = wynik musi byc identyczny co do bajtu,
    //   n  = dopuszczalne |roznica| <= n (np. GPU: inne zaokraglenie sqrt),
    //  -1  = wariant przyblizony – roznice tylko raportowane, nie sa bledem.
    int         maxAllowedDiff;
    KernelInfo  kernel;        // tylko warianty 1-watkowe CPU
};

constexpr int kApproximate = -1;

const std::vector<VariantInfo>& allVariants();
const VariantInfo&              variantInfo(VariantId id);
const VariantInfo*              findVariant(const std::string& name);   // nullptr gdy brak

// Czy wariant moze byc jadrem dla T (1-watkowy, CPU, dostepny)
bool isKernelVariant(const VariantInfo& v);

// Trzyma zasoby wspoldzielone miedzy kolejnymi wywolaniami (pula watkow,
// bufory wariantow z reuseBuffers), zeby ich tworzenie nie wchodzilo do
// pomiaru kolejnych powtorzen.
class VariantRunner {
public:
    VariantRunner();
    ~VariantRunner();

    // Rzuca std::runtime_error dla wariantu niedostepnego.
    RunResult run(VariantId id, const GrayImage& img, const Mask& mask,
                  const RunConfig& cfg = {});

    // Oddaje bufor wyniku do ponownego uzycia (warianty z reuseBuffers nie alokuja
    // i nie zeruja go przy kolejnym wywolaniu).  Po wywolaniu r.output jest pusty.
    void recycle(RunResult& r) { if (!r.output.empty()) outBuf_ = std::move(r.output); r.output.clear(); }

private:
    EdgeDetector& detectorFor(const RunConfig& cfg);
    void runSingle(const VariantInfo& v, const GrayImage& img, const Mask& mask,
                   const RunConfig& cfg, RunResult& r);

    std::unique_ptr<EdgeDetector> detector_;
    Scratch                       scratch_;   // dla jader z reuseBuffers
    AlignedVec<float>             mag_;
    std::vector<Scratch>          ompScratch_;
    std::vector<std::uint8_t>     outBuf_;    // bufor wyniku dla wariantow z reuseBuffers
    AlignedVec<float>             ompMag_;
};

// OpenMP (core/variants/omp.cpp)
bool openmpAvailable();
void runOpenMP(const KernelInfo& k, const GrayImage& img, const Mask& mask, const RunConfig& cfg,
               std::vector<Scratch>& scratchPool, AlignedVec<float>& magPool, RunResult& r);

// Wygodna funkcja: wspolny VariantRunner dla biezacego watku.
RunResult runVariant(VariantId id, const GrayImage& img, const Mask& mask,
                     const RunConfig& cfg = {});
