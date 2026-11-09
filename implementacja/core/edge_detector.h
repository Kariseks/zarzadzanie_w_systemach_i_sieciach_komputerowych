// ============================================================================
//  edge_detector.h  –  klasa EdgeDetector: wielowatkowe wykrywanie krawedzi
//
//  Uzywane przez wariant "T" (zob. variants.h).  Liczy dowolnym jadrem
//  (KernelInfo z kernels.h): scheduler rozdaje fragmenty wierszy, a kazdy
//  watek wola jadro dla swojego fragmentu z wlasna pamiecia robocza.
//
//  GlobalMax: 2 etapy (splot + maksimum, bariera, redukcja, normalizacja).
//  Fixed / Clamp: 1 etap, bez bariery i redukcji.
//
//  Parametry mechanizmu (do eksperymentow E6, E8, E10):
//    mech     – pool (watki tworzone raz) / spawn (nowe watki w kazdym etapie)
//    reduce   – sposob wyznaczenia globalnego maksimum (tylko GlobalMax):
//      local    : maksimum fragmentu do wlasnej komorki watku (osobna linia
//                 cache), redukcja po barierze – brak wspolzawodnictwa
//      atomic   : kazdy piksel porownywany ze wspolnym std::atomic<float>,
//                 zapis CAS tylko gdy wiekszy (wspolny zasob, rzadkie zapisy)
//      mutex    : kazdy piksel aktualizuje wspolne maksimum pod std::mutex
//                 (naiwna synchronizacja – koszt blokady na piksel)
//      px_pad   : kazdy piksel zapisuje biezace maksimum do wlasnej komorki
//                 watku (osobna linia cache, alignas(64))
//      px_nopad : jak px_pad, ale komorki watkow obok siebie w jednej linii
//                 cache -> false sharing
//    affinity – przypinanie watkow do rdzeni (topology.h)
//
//  Przyklad:
//      EdgeDetector det({ 8, Schedule::Dynamic, 16 });   // 8 watkow
//      std::vector<std::uint8_t> edges(w * h);
//      det.process(img, mask, Norm::L2, Scaling::GlobalMax, { rowsV0, false }, Reduce::Local, edges);
//      det.lastReport();   // czasy etapow + statystyki kazdego watku
// ============================================================================
#pragma once

#include <memory>
#include <string>

#include "image.h"
#include "kernels.h"
#include "masks.h"
#include "measure.h"
#include "row_scheduler.h"
#include "thread_pool.h"
#include "topology.h"

enum class ThreadMech { Pool, Spawn };
enum class Reduce     { Local, Atomic, Mutex, PixelPadded, PixelUnpadded };

const char* toString(ThreadMech m);
const char* toString(Reduce r);
bool parseThreadMech(const std::string& s, ThreadMech& out);   // pool | spawn
bool parseReduce(const std::string& s, Reduce& out);           // local | atomic | mutex | px_pad | px_nopad

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
    int        threads   = 0;                  // 0 = liczba rdzeni logicznych
    Schedule   schedule  = Schedule::Dynamic;  // strategia podzialu pracy
    int        chunkRows = 16;                 // rozmiar fragmentu (wiersze)
    ThreadMech mech      = ThreadMech::Pool;
    Affinity   affinity  = Affinity::None;
};

class EdgeDetector {
public:
    using Config = EdgeDetectorConfig;

    explicit EdgeDetector(Config cfg = {});

    // Uzupelnia wartosci domyslne: threads <= 0 -> liczba rdzeni logicznych, chunk >= 1
    static Config normalize(Config c);

    // Glowna metoda: obraz szary -> mapa krawedzi 0..255 do 'out' (rozmiar w*h,
    // przydzielony przez wywolujacego – VariantRunner decyduje, czy bufor jest nowy, czy z poprzedniego wywolania)
    void process(const GrayImage& img, const Mask& mask, Norm norm, Scaling scaling,
                 const KernelInfo& kernel, Reduce reduce, std::vector<std::uint8_t>& out);

    const Config& config() const { return cfg_; }
    int      threadCount() const { return cfg_.threads; }
    Schedule schedule()    const { return cfg_.schedule; }
    int      chunkRows()   const { return cfg_.chunkRows; }

    const DetectorReport& lastReport() const { return report_; }

private:
    // Uruchamia etap: scheduler rozdaje wiersze, watki wolaja
    // rowFn(threadId, y0, y1) dla kazdego otrzymanego fragmentu.
    template <class RowFn>
    PhaseStats runPhase(int totalRows, RowFn&& rowFn);

    Config                      cfg_;
    std::vector<int>            cpus_;
    std::unique_ptr<ThreadPool> pool_;      // nullptr dla mech = spawn
    DetectorReport              report_;
    std::vector<Scratch>        scratch_;   // pamiec robocza per watek (KernelInfo::reuseBuffers)
    AlignedVec<float>           mag_;       // bufor modulu (KernelInfo::reuseBuffers)
};
