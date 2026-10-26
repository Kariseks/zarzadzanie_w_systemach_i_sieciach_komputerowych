// ============================================================================
//  Wielowatkowe wykrywanie krawedzi – program terminalowy
//
//  Uzycie:
//    edge_detector                      -> menu interaktywne
//    edge_detector [opcje]
//
//  Opcje:
//    -m N       maska 1..5 (1 = Sobel 3x3)
//    -t N       liczba watkow (domyslnie: liczba rdzeni logicznych)
//    -s NAZWA   harmonogram: static | cyclic | dynamic | guided
//    -c N       rozmiar fragmentu w wierszach (domyslnie 16)
//    -i PLIK    obraz wejsciowy  (domyslnie input.jpg)
//    -o PLIK    obraz wyjsciowy  (domyslnie output.jpg)
//    -b         benchmark: porownanie 1, 2, 4, ... watkow
//    -r N       powtorzenia w benchmarku (domyslnie 3, liczy sie najlepszy)
//    -h         pomoc
// ============================================================================
#include "edge_detector.h"

#include <thread>

struct Options {
    int         mask      = 1;
    int         threads   = 0;       // 0 = auto
    Schedule    schedule  = Schedule::Dynamic;
    int         chunk     = 16;
    std::string input     = "input.jpg";
    std::string output    = "output.jpg";
    bool        benchmark = false;
    int         repeats   = 3;
};

static int hwThreads()
{
    const int n = static_cast<int>(std::thread::hardware_concurrency());
    return n > 0 ? n : 1;
}

static void printHelp()
{
    std::cout <<
        "Uzycie: edge_detector [opcje]   (bez opcji = menu)\n"
        "  -m N      maska 1..5\n"
        "  -t N      liczba watkow (domyslnie " << hwThreads() << ")\n"
        "  -s NAZWA  static | cyclic | dynamic | guided\n"
        "  -c N      rozmiar fragmentu w wierszach (domyslnie 16)\n"
        "  -i PLIK   wejscie (input.jpg)\n"
        "  -o PLIK   wyjscie (output.jpg)\n"
        "  -b        benchmark 1..N watkow\n"
        "  -r N      powtorzenia w benchmarku (domyslnie 3)\n";
}

// Czyta linie; pusta linia -> wartosc domyslna
static std::string ask(const std::string& prompt)
{
    std::cout << prompt;
    std::string line;
    std::getline(std::cin, line);
    return line;
}

static Options interactiveMenu(const std::vector<Mask>& masks)
{
    Options o;
    std::cout << "=== Wykrywanie krawedzi – konfiguracja ===\n\nMaski:\n";
    for (size_t i = 0; i < masks.size(); ++i)
        std::cout << "  " << (i + 1) << ") " << masks[i].name << "\n";
    std::string s = ask("Maska [Enter = 1]: ");
    if (!s.empty()) o.mask = std::atoi(s.c_str());

    s = ask("Liczba watkow [Enter = " + std::to_string(hwThreads()) + "]: ");
    if (!s.empty()) o.threads = std::atoi(s.c_str());

    std::cout << "Harmonogram:\n"
                 "  1) static   – rowne bloki, jeden na watek\n"
                 "  2) cyclic   – fragmenty rozdawane po kolei (round-robin)\n"
                 "  3) dynamic  – wolny watek bierze kolejny fragment\n"
                 "  4) guided   – jak dynamic, fragmenty coraz mniejsze\n";
    s = ask("Harmonogram [Enter = 3]: ");
    if (!s.empty() && !parseSchedule(s, o.schedule))
        std::cout << "Nieznany harmonogram – uzywam dynamic\n";

    if (o.schedule != Schedule::Static) {
        s = ask("Rozmiar fragmentu w wierszach [Enter = 16]: ");
        if (!s.empty()) o.chunk = std::atoi(s.c_str());
    }

    s = ask("Benchmark 1..N watkow zamiast pojedynczego przebiegu? (t/N): ");
    o.benchmark = !s.empty() && (s[0] == 't' || s[0] == 'T' || s[0] == 'y' || s[0] == 'Y');
    return o;
}

static bool parseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Brak wartosci dla " << name << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if      (a == "-m") o.mask    = std::atoi(value("-m"));
        else if (a == "-t") o.threads = std::atoi(value("-t"));
        else if (a == "-c") o.chunk   = std::atoi(value("-c"));
        else if (a == "-r") o.repeats = std::atoi(value("-r"));
        else if (a == "-i") o.input   = value("-i");
        else if (a == "-o") o.output  = value("-o");
        else if (a == "-b") o.benchmark = true;
        else if (a == "-s") {
            if (!parseSchedule(value("-s"), o.schedule)) {
                std::cerr << "Nieznany harmonogram\n";
                return false;
            }
        }
        else if (a == "-h" || a == "--help") { printHelp(); std::exit(0); }
        else { std::cerr << "Nieznana opcja: " << a << "\n"; printHelp(); return false; }
    }
    return true;
}

static void printThreadTable(const std::string& title, const PhaseStats& ps)
{
    long long totalRows = 0;
    for (const auto& t : ps.perThread) totalRows += t.rows;

    std::cout << "\n" << title << " – podzial pracy miedzy watki:\n"
              << std::right
              << std::setw(7)  << "Watek"
              << std::setw(12) << "Fragmenty"
              << std::setw(10) << "Wiersze"
              << std::setw(9)  << "Udzial"
              << std::setw(14) << "CPU [ms]"
              << std::setw(16) << "Koniec po [ms]" << "\n"
              << std::string(68, '-') << "\n";

    for (size_t i = 0; i < ps.perThread.size(); ++i) {
        const ThreadStats& t = ps.perThread[i];
        const double share = totalRows ? 100.0 * t.rows / totalRows : 0.0;
        std::cout << std::setw(7)  << i
                  << std::setw(12) << t.chunks
                  << std::setw(10) << t.rows
                  << std::fixed << std::setprecision(1)
                  << std::setw(8)  << share << "%"
                  << std::setprecision(3)
                  << std::setw(14) << t.cpuMs
                  << std::setw(16) << t.busyMs << "\n";
    }
}

// ----------------------------------------------------------------------------
//  Pojedynczy przebieg z pelnym raportem
// ----------------------------------------------------------------------------
static int runSingle(const Options& o, const Mask& mask)
{
    EdgeDetector det({ o.threads, o.schedule, o.chunk });

    std::cout << "\n=== Wykrywanie krawedzi ===\n"
              << "Maska:        " << mask.name << "\n"
              << "Watki:        " << det.threadCount()
              << "  (rdzenie logiczne w systemie: " << hwThreads() << ")\n"
              << "Harmonogram:  " << toString(det.schedule());
    if (det.schedule() != Schedule::Static)
        std::cout << ", fragment " << det.chunkRows() << " wierszy";
    std::cout << "\nRAM na starcie (z pula watkow): " << mb(currentRamBytes()) << "\n";

    Stopwatch total;
    printHeader();

    Stopwatch sw;
    GrayImage img;
    if (!loadGray(o.input, img)) return 1;
    printStage("Wczytanie JPG", sw);

    std::vector<std::uint8_t> edges = det.process(img, mask);
    const DetectorReport& rep = det.lastReport();
    printRow("Splot (maska)", rep.convolution.wallMs,   rep.convolution.cpuMs);
    printRow("Normalizacja",  rep.normalization.wallMs, rep.normalization.cpuMs);

    sw.restart();
    if (!saveGrayJpg(o.output, edges, img.width, img.height)) {
        std::cerr << "Blad zapisu '" << o.output << "'\n";
        return 1;
    }
    printStage("Zapis JPG", sw);

    std::cout << std::string(80, '-') << "\n";
    printStage("RAZEM", total);

    printThreadTable("Splot", rep.convolution);

    const size_t buffers = img.data.size()                      // wejscie
                         + img.data.size() * sizeof(float)      // modul gradientu
                         + edges.size();                        // wyjscie
    std::cout << "\nObraz:            " << img.width << " x " << img.height << " px\n"
              << "Bufory obrazu:    " << mb(buffers) << "\n"
              << "RAM szczytowy:    " << mb(peakRamBytes()) << "\n"
              << "Zapisano:         " << o.output << "\n";
    return 0;
}

// ----------------------------------------------------------------------------
//  Benchmark: ta sama maska i harmonogram dla 1, 2, 4, ... watkow
// ----------------------------------------------------------------------------
static int runBenchmark(const Options& o, const Mask& mask)
{
    GrayImage img;
    if (!loadGray(o.input, img)) return 1;

    std::vector<int> counts;
    const int maxT = std::max(o.threads > 0 ? o.threads : hwThreads(), 1);
    for (int t = 1; t <= maxT; t *= 2) counts.push_back(t);
    if (counts.back() != maxT) counts.push_back(maxT);

    std::cout << "\n=== Benchmark ===\n"
              << "Obraz: " << img.width << " x " << img.height << " px,  maska: " << mask.name
              << ",  harmonogram: " << toString(o.schedule)
              << ",  powtorzenia: " << o.repeats << " (brany najlepszy)\n\n"
              << std::right
              << std::setw(7)  << "Watki"
              << std::setw(14) << "Zegar [ms]"
              << std::setw(14) << "CPU [ms]"
              << std::setw(12) << "CPU/zegar"
              << std::setw(14) << "Przyspiesz."
              << std::setw(13) << "Efektywn."
              << std::setw(14) << "RAM" << "\n"
              << std::string(88, '-') << "\n";

    double baseWall = 0.0;
    std::vector<std::uint8_t> lastEdges;

    for (int t : counts) {
        EdgeDetector det({ t, o.schedule, o.chunk });
        double bestWall = 1e300, bestCpu = 0.0;
        for (int r = 0; r < std::max(1, o.repeats); ++r) {
            lastEdges = det.process(img, mask);
            const DetectorReport& rep = det.lastReport();
            const double wall = rep.convolution.wallMs + rep.normalization.wallMs;
            const double cpu  = rep.convolution.cpuMs  + rep.normalization.cpuMs;
            if (wall < bestWall) { bestWall = wall; bestCpu = cpu; }
        }
        if (t == 1) baseWall = bestWall;
        const double speedup = bestWall > 0.0 ? baseWall / bestWall : 0.0;

        std::cout << std::setw(7)  << t
                  << std::fixed << std::setprecision(3)
                  << std::setw(14) << bestWall
                  << std::setw(14) << bestCpu
                  << std::setprecision(2)
                  << std::setw(12) << bestCpu / bestWall
                  << std::setw(13) << speedup << "x"
                  << std::setw(12) << 100.0 * speedup / t << "%"
                  << std::setw(14) << mb(currentRamBytes()) << "\n";
    }

    saveGrayJpg(o.output, lastEdges, img.width, img.height);
    std::cout << "\nPrzyspieszenie = czas(1 watek) / czas(N watkow)\n"
              << "Efektywnosc    = przyspieszenie / N  (100% = idealne skalowanie)\n"
              << "Czasy dotycza samego przetwarzania (bez wczytania/zapisu JPG).\n"
              << "RAM szczytowy: " << mb(peakRamBytes()) << "\n"
              << "Zapisano: " << o.output << "\n";
    return 0;
}

int main(int argc, char** argv)
{
    const std::vector<Mask> masks = availableMasks();

    Options o;
    if (argc > 1) {
        if (!parseArgs(argc, argv, o)) return 1;
    } else {
        o = interactiveMenu(masks);
    }

    if (o.mask < 1 || o.mask > static_cast<int>(masks.size())) {
        std::cout << "Niepoprawna maska – uzywam 1 (" << masks[0].name << ")\n";
        o.mask = 1;
    }
    const Mask& mask = masks[static_cast<size_t>(o.mask - 1)];

    return o.benchmark ? runBenchmark(o, mask) : runSingle(o, mask);
}
