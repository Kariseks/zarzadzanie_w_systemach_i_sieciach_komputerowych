// ============================================================================
//  edge_detector.h  –  klasa EdgeDetector: wielowatkowe wykrywanie krawedzi
//
//  Przyklad:
//      EdgeDetector det({ 8, Schedule::Dynamic, 16 });   // 8 watkow
//      std::vector<uint8_t> edges = det.process(img, mask);
//      det.lastReport();   // czasy etapow + statystyki kazdego watku
// ============================================================================
#pragma once

#include "common.h"
#include "row_scheduler.h"
#include "thread_pool.h"

// Statystyki jednego watku w jednym etapie
struct ThreadStats {
    int       chunks = 0;    // ile fragmentow dostal od schedulera
    long long rows   = 0;    // ile wierszy policzyl
    double    cpuMs  = 0.0;  // czas CPU tego watku
    double    busyMs = 0.0;  // czas zegarowy od startu do konca jego pracy
};

// Statystyki etapu (splot lub normalizacja)
struct PhaseStats {
    double wallMs = 0.0;     // czas zegarowy etapu
    double cpuMs  = 0.0;     // czas CPU calego procesu w tym etapie
    std::vector<ThreadStats> perThread;
};

struct DetectorReport {
    PhaseStats convolution;
    PhaseStats normalization;
    float      maxGradient = 0.0f;
};

// Konfiguracja detektora (poza klasa, aby mozna jej bylo uzyc jako
// argumentu domyslnego konstruktora)
struct EdgeDetectorConfig {
    int      threads   = 0;                  // 0 = liczba rdzeni logicznych
    Schedule schedule  = Schedule::Dynamic;  // strategia podzialu pracy
    int      chunkRows = 16;                 // rozmiar fragmentu (wiersze)
};

class EdgeDetector {
public:
    using Config = EdgeDetectorConfig;

    explicit EdgeDetector(Config cfg = {});

    // Glowna metoda: obraz szary -> mapa krawedzi 0..255 (ten sam rozmiar)
    std::vector<std::uint8_t> process(const GrayImage& img, const Mask& mask);

    int      threadCount() const { return cfg_.threads; }
    Schedule schedule()    const { return cfg_.schedule; }
    int      chunkRows()   const { return cfg_.chunkRows; }

    const DetectorReport& lastReport() const { return report_; }

private:
    static Config normalize(Config c);

    // Uruchamia etap: scheduler rozdaje wiersze, watki puli wolaja
    // rowFn(threadId, y0, y1) dla kazdego otrzymanego fragmentu.
    template <class RowFn>
    PhaseStats runPhase(int totalRows, RowFn&& rowFn);

    Config         cfg_;    // UWAGA: przed pool_, bo pool_ korzysta z cfg_
    ThreadPool     pool_;
    DetectorReport report_;
};
