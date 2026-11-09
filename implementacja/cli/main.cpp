// ============================================================================
//  edge_cli – wykrywanie krawedzi, program terminalowy do recznych prob
//
//  Uzycie:
//    edge_cli                      -> menu interaktywne
//    edge_cli [opcje]
//
//  Opcje:
//    -v NAZWA   wariant kodu (domyslnie T – wielowatkowy); lista: --list-variants
//    -k NAZWA   jadro dla wariantu T (domyslnie V0): V0..V4
//    -n NORMA   l2 | l1 | linf            (domyslnie l2)
//    -S SKALA   global | fixed | clamp    (domyslnie global)
//    -m MASKA   maska: numer 1..5 albo id (sobel3, sobel5, prewitt, scharr, roberts)
//    -t N       liczba watkow (domyslnie: liczba rdzeni logicznych)
//    -s NAZWA   harmonogram: static | cyclic | dynamic | guided
//    -c N       rozmiar fragmentu w wierszach (domyslnie 16)
//    -i PLIK    obraz wejsciowy  (domyslnie input.jpg)
//    -o PLIK    obraz wyjsciowy  (domyslnie output.jpg; .png = bezstratnie)
//    -b         benchmark: porownanie 1, 2, 4, ... watkow
//    -r N       powtorzenia w benchmarku (domyslnie 3, liczy sie najlepszy)
//    --list-variants   lista wariantow i ich dostepnosc
//    --verify [-v A,B] [-m M,N] [-i ZRODLO]...
//               sprawdza warianty wzgledem V0 (domyslnie wszystkie dostepne,
//               zestaw obrazow z przypadkami brzegowymi); kod wyjscia 3 = blad
//
//  ZRODLO obrazu: plik albo synthetic:WxH[:ziarno], np. synthetic:3840x2160:7
//    -h         pomoc
//
//  Pomiary do sprawozdania robi edge_bench + skrypty Pythona; ten program
//  sluzy do szybkiego sprawdzenia dzialania i podgladu wynikow.
// ============================================================================
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "filter.h"
#include "image.h"
#include "masks.h"
#include "measure.h"
#include "synthetic.h"
#include "variants.h"
#include "verify.h"

struct Options {
    std::string variant   = "T";
    std::string kernel    = "V0";
    Norm        norm      = Norm::L2;
    Scaling     scaling   = Scaling::GlobalMax;
    std::string mask      = "1";
    int         threads   = 0;       // 0 = auto
    Schedule    schedule  = Schedule::Dynamic;
    int         chunk     = 16;
    std::string input     = "input.jpg";
    std::string output    = "output.jpg";
    bool        benchmark = false;
    int         repeats   = 3;
    bool        verify    = false;
    std::vector<std::string> verifyImages;   // kolejne -i w trybie --verify
};

static std::vector<std::string> splitComma(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t pos = s.find(',', start);
        const std::string item = s.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!item.empty()) out.push_back(item);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

static int hwThreads()
{
    const int n = static_cast<int>(std::thread::hardware_concurrency());
    return n > 0 ? n : 1;
}

static void printHelp()
{
    std::cout <<
        "Uzycie: edge_cli [opcje]   (bez opcji = menu)\n"
        "  -v NAZWA  wariant kodu (domyslnie T), lista: --list-variants\n"
        "  -k NAZWA  jadro dla T (domyslnie V0)\n"
        "  -n NORMA  l2 | l1 | linf\n"
        "  -S SKALA  global | fixed | clamp\n"
        "  -m MASKA  1..5 albo sobel3 | sobel5 | prewitt | scharr | roberts\n"
        "  -t N      liczba watkow (domyslnie " << hwThreads() << ")\n"
        "  -s NAZWA  static | cyclic | dynamic | guided\n"
        "  -c N      rozmiar fragmentu w wierszach (domyslnie 16)\n"
        "  -i PLIK   wejscie (input.jpg)\n"
        "  -o PLIK   wyjscie (output.jpg, .png = bezstratnie)\n"
        "  -b        benchmark 1..N watkow\n"
        "  -r N      powtorzenia w benchmarku (domyslnie 3)\n"
        "  --list-variants  lista wariantow\n"
        "  --verify  sprawdzenie wariantow wzgledem V0 (-v A,B  -m M,N  -i ZRODLO ...)\n"
        "  ZRODLO obrazu: plik albo synthetic:WxH[:ziarno]\n";
}

static void printVariants()
{
    std::cout << std::left << std::setw(6) << "ID" << std::setw(11) << "Watki"
              << std::setw(12) << "Dostepny" << "Opis\n" << std::string(90, '-') << "\n";
    for (const auto& v : allVariants()) {
        std::cout << std::left << std::setw(6) << v.name
                  << std::setw(11) << (v.multithreaded ? "wiele" : "1")
                  << std::setw(12) << (v.available ? "tak" : "nie")
                  << v.description;
        if (!v.available) std::cout << "  [" << v.unavailableReason << "]";
        std::cout << "\n";
    }
}

static RunConfig makeConfig(const Options& o, int threads)
{
    RunConfig c;
    c.threads   = threads;
    c.schedule  = o.schedule;
    c.chunkRows = o.chunk;
    c.norm      = o.norm;
    c.scaling   = o.scaling;
    const VariantInfo* k = findVariant(o.kernel);
    if (!k || !isKernelVariant(*k)) throw std::runtime_error("niepoprawne jadro: " + o.kernel);
    c.kernel = k->id;
    return c;
}

static bool saveOutput(const std::string& path, const std::vector<std::uint8_t>& data, int w, int h)
{
    const bool png = path.size() >= 4 && path.compare(path.size() - 4, 4, ".png") == 0;
    return png ? saveGrayPng(path, data, w, h) : saveGrayJpg(path, data, w, h);
}

// ----------------------------------------------------------------------------
//  Tabela etapow
// ----------------------------------------------------------------------------
static void printHeader()
{
    std::cout << "\n"
              << std::left  << std::setw(24) << "Etap"
              << std::right << std::setw(14) << "Zegar [ms]"
              << std::setw(14) << "CPU [ms]"
              << std::setw(12) << "CPU/zegar"
              << std::setw(16) << "RAM (teraz)" << "\n"
              << std::string(80, '-') << "\n";
}

// CPU/zegar ~ ile rdzeni srednio pracowalo (1.0 = jeden rdzen, 4.0 = cztery)
static void printRow(const std::string& name, double wallMs, double cpuMs)
{
    std::cout << std::left  << std::setw(24) << name
              << std::right << std::fixed << std::setprecision(3)
              << std::setw(14) << wallMs;
    if (cpuMs >= 0.0)
        std::cout << std::setw(14) << cpuMs
                  << std::setprecision(2)
                  << std::setw(12) << (wallMs > 0.0 ? cpuMs / wallMs : 0.0);
    else
        std::cout << std::setw(14) << "-" << std::setw(12) << "-";
    std::cout << std::setw(16) << mb(currentRamBytes()) << "\n";
}

static void printStage(const std::string& name, const Stopwatch& sw)
{
    printRow(name, sw.wallMs(), sw.cpuMs());
}

static void printThreadTable(const std::string& title, const std::vector<ThreadStats>& perThread)
{
    long long totalRows = 0;
    for (const auto& t : perThread) totalRows += t.rows;

    std::cout << "\n" << title << " – podzial pracy miedzy watki:\n"
              << std::right
              << std::setw(7)  << "Watek"
              << std::setw(12) << "Fragmenty"
              << std::setw(10) << "Wiersze"
              << std::setw(9)  << "Udzial"
              << std::setw(14) << "CPU [ms]"
              << std::setw(16) << "Koniec po [ms]" << "\n"
              << std::string(68, '-') << "\n";

    for (size_t i = 0; i < perThread.size(); ++i) {
        const ThreadStats& t = perThread[i];
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
//  Menu i argumenty
// ----------------------------------------------------------------------------
// Czyta linie; pusta linia -> wartosc domyslna
static std::string ask(const std::string& prompt)
{
    std::cout << prompt;
    std::string line;
    std::getline(std::cin, line);
    return line;
}

static Options interactiveMenu()
{
    Options o;
    std::cout << "=== Wykrywanie krawedzi – konfiguracja ===\n\nWarianty:\n";
    for (const auto& v : allVariants())
        if (v.available) std::cout << "  " << std::left << std::setw(4) << v.name << v.description << "\n";
    std::string s = ask("Wariant [Enter = T]: ");
    if (!s.empty()) o.variant = s;

    std::cout << "\nMaski:\n";
    const auto& masks = availableMasks();
    for (size_t i = 0; i < masks.size(); ++i)
        std::cout << "  " << (i + 1) << ") " << masks[i].name << "\n";
    s = ask("Maska [Enter = 1]: ");
    if (!s.empty()) o.mask = s;

    const VariantInfo* vi = findVariant(o.variant);
    if (vi && vi->multithreaded) {
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
    }
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
        if      (a == "-v") o.variant = value("-v");
        else if (a == "-k") o.kernel  = value("-k");
        else if (a == "-n") { if (!parseNorm(value("-n"), o.norm)) { std::cerr << "Nieznana norma\n"; return false; } }
        else if (a == "-S") { if (!parseScaling(value("-S"), o.scaling)) { std::cerr << "Nieznane skalowanie\n"; return false; } }
        else if (a == "-m") o.mask    = value("-m");
        else if (a == "-t") o.threads = std::atoi(value("-t"));
        else if (a == "-c") o.chunk   = std::atoi(value("-c"));
        else if (a == "-r") o.repeats = std::atoi(value("-r"));
        else if (a == "-i") { o.input = value("-i"); o.verifyImages.push_back(o.input); }
        else if (a == "-o") o.output  = value("-o");
        else if (a == "-b") o.benchmark = true;
        else if (a == "-s") {
            if (!parseSchedule(value("-s"), o.schedule)) {
                std::cerr << "Nieznany harmonogram\n";
                return false;
            }
        }
        else if (a == "--list-variants") { printVariants(); std::exit(0); }
        else if (a == "--verify") o.verify = true;
        else if (a == "-h" || a == "--help") { printHelp(); std::exit(0); }
        else { std::cerr << "Nieznana opcja: " << a << "\n"; printHelp(); return false; }
    }
    return true;
}

// ----------------------------------------------------------------------------
//  Pojedynczy przebieg z pelnym raportem
// ----------------------------------------------------------------------------
static int runSingle(const Options& o, const VariantInfo& v, const Mask& mask)
{
    const RunConfig cfg = makeConfig(o, o.threads);

    std::cout << "\n=== Wykrywanie krawedzi ===\n"
              << "Wariant:      " << v.name << " – " << v.description << "\n"
              << "Maska:        " << mask.name << "\n"
              << "Norma:        " << toString(o.norm) << ",  skalowanie: " << toString(o.scaling) << "\n";
    if (v.multithreaded) {
        std::cout << "Jadro:        " << o.kernel << "\n";
        std::cout << "Watki:        " << (o.threads > 0 ? o.threads : hwThreads())
                  << "  (rdzenie logiczne w systemie: " << hwThreads() << ")\n"
                  << "Harmonogram:  " << toString(o.schedule);
        if (o.schedule != Schedule::Static)
            std::cout << ", fragment " << o.chunk << " wierszy";
        std::cout << "\n";
    }

    Stopwatch total;
    printHeader();

    Stopwatch sw;
    GrayImage img;
    if (!loadOrGenerate(o.input, img)) return 1;
    printStage("Wczytanie obrazu", sw);

    VariantRunner runner;
    RunResult r = runner.run(v.id, img, mask, cfg);
    for (const auto& [stage, ms] : r.stagesMs)
        printRow("  etap: " + stage, ms, -1.0);
    printRow("Przetwarzanie (" + std::string(v.name) + ")", r.wallMs, r.cpuMs);

    sw.restart();
    if (!saveOutput(o.output, r.output, img.width, img.height)) {
        std::cerr << "Blad zapisu '" << o.output << "'\n";
        return 1;
    }
    printStage("Zapis obrazu", sw);

    std::cout << std::string(80, '-') << "\n";
    printStage("RAZEM", total);

    if (!r.perThread.empty()) {
        printThreadTable("Splot", r.perThread);
        std::cout << "Nierownowaga (koniec ostatniego - pierwszego watku): "
                  << std::fixed << std::setprecision(3) << r.imbalanceMs << " ms\n";
    }

    std::cout << "\nObraz:            " << img.width << " x " << img.height << " px\n"
              << "RAM szczytowy:    " << mb(peakRamBytes()) << "\n"
              << "Zapisano:         " << o.output << "\n";
    return 0;
}

// ----------------------------------------------------------------------------
//  Benchmark: ten sam wariant dla 1, 2, 4, ... watkow
// ----------------------------------------------------------------------------
static int runBenchmark(const Options& o, const VariantInfo& v, const Mask& mask)
{
    GrayImage img;
    if (!loadOrGenerate(o.input, img)) return 1;

    std::vector<int> counts{ 1 };
    if (v.multithreaded) {
        const int maxT = std::max(o.threads > 0 ? o.threads : hwThreads(), 1);
        for (int t = 2; t <= maxT; t *= 2) counts.push_back(t);
        if (counts.back() != maxT) counts.push_back(maxT);
    }

    std::cout << "\n=== Benchmark ===\n"
              << "Obraz: " << img.width << " x " << img.height << " px,  wariant: " << v.name
              << ",  maska: " << mask.name;
    if (v.multithreaded) std::cout << ",  harmonogram: " << toString(o.schedule);
    std::cout << ",  powtorzenia: " << o.repeats << " (brany najlepszy)\n\n"
              << std::right
              << std::setw(7)  << "Watki"
              << std::setw(14) << "Zegar [ms]"
              << std::setw(14) << "CPU [ms]"
              << std::setw(12) << "CPU/zegar"
              << std::setw(14) << "Przyspiesz."
              << std::setw(13) << "Efektywn."
              << std::setw(14) << "RAM" << "\n"
              << std::string(88, '-') << "\n";

    VariantRunner runner;
    double baseWall = 0.0;
    std::vector<std::uint8_t> lastEdges;

    for (int t : counts) {
        const RunConfig cfg = makeConfig(o, t);
        double bestWall = 1e300, bestCpu = 0.0;
        for (int rep = 0; rep < std::max(1, o.repeats); ++rep) {
            RunResult r = runner.run(v.id, img, mask, cfg);
            if (r.wallMs < bestWall) { bestWall = r.wallMs; bestCpu = r.cpuMs; }
            lastEdges = std::move(r.output);
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

    saveOutput(o.output, lastEdges, img.width, img.height);
    std::cout << "\nPrzyspieszenie = czas(1 watek) / czas(N watkow)\n"
              << "Efektywnosc    = przyspieszenie / N  (100% = idealne skalowanie)\n"
              << "Czasy dotycza samego przetwarzania (bez wczytania/zapisu obrazu).\n"
              << "RAM szczytowy: " << mb(peakRamBytes()) << "\n"
              << "Zapisano: " << o.output << "\n";
    return 0;
}

// ----------------------------------------------------------------------------
//  Weryfikacja poprawnosci wszystkich wariantow wzgledem V0
// ----------------------------------------------------------------------------
static int runVerify(const Options& o, bool variantGiven, bool maskGiven, bool kernelGiven)
{
    VerifyOptions vo;
    if (variantGiven) vo.variants = splitComma(o.variant);
    if (maskGiven)    vo.masks    = splitComma(o.mask);
    if (kernelGiven)  vo.kernels  = splitComma(o.kernel);
    vo.images = o.verifyImages;

    std::cout << "=== Weryfikacja wariantow wzgledem V0 ===\n";
    Stopwatch sw;
    std::vector<VerifyCase> cases;
    try {
        cases = runVerification(vo, [](size_t done, size_t total) {
            if (done % 50 == 0 || done == total)
                std::cerr << "\r  " << done << " / " << total << std::flush;
        });
    } catch (const std::exception& e) {
        std::cerr << "\nBlad: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "\n";

    struct Summary { size_t n = 0, pass = 0; int maxDiff = 0; double minPsnr = std::numeric_limits<double>::infinity(); };
    std::vector<std::pair<std::string, Summary>> summary;
    for (const auto& c : cases) {
        const std::string key = c.kernel.empty() ? c.variant : c.variant + "/" + c.kernel;
        auto it = std::find_if(summary.begin(), summary.end(),
                               [&](const auto& p) { return p.first == key; });
        if (it == summary.end()) { summary.push_back({ key, {} }); it = summary.end() - 1; }
        Summary& s = it->second;
        ++s.n;
        if (c.pass) ++s.pass;
        s.maxDiff = std::max(s.maxDiff, c.cmp.maxAbsDiff);
        s.minPsnr = std::min(s.minPsnr, c.cmp.psnrDb);
    }

    std::cout << "\n" << std::left << std::setw(8) << "Wariant" << std::right
              << std::setw(12) << "Przypadki" << std::setw(10) << "OK"
              << std::setw(14) << "Maks. roznica" << std::setw(16) << "Min. PSNR [dB]"
              << std::setw(12) << "Tolerancja" << "\n" << std::string(72, '-') << "\n";
    for (const auto& [name, s] : summary) {
        const VariantInfo* v = findVariant(name.substr(0, name.find('/')));
        std::cout << std::left << std::setw(8) << name << std::right
                  << std::setw(12) << s.n << std::setw(10) << s.pass
                  << std::setw(14) << s.maxDiff << std::setw(16) << formatPsnr(s.minPsnr)
                  << std::setw(12) << (v->maxAllowedDiff == kApproximate ? std::string("przybl.")
                                                                         : std::to_string(v->maxAllowedDiff))
                  << "\n";
    }

    size_t failures = 0;
    for (const auto& c : cases) {
        if (c.pass) continue;
        if (++failures <= 20) {
            std::cout << "BLAD " << c.variant << (c.kernel.empty() ? "" : "/" + c.kernel) << "  " << c.image
                      << " (" << c.width << "x" << c.height << ")  maska " << c.mask
                      << " " << toString(c.cfg.norm) << " " << toString(c.cfg.scaling);
            if (c.multithreaded)
                std::cout << "  watki " << c.cfg.threads << " " << toString(c.cfg.schedule)
                          << " chunk " << c.cfg.chunkRows;
            if (!c.error.empty()) std::cout << "  wyjatek: " << c.error;
            else std::cout << "  maks. roznica " << c.cmp.maxAbsDiff << ", rozne piksele "
                           << c.cmp.differing << (c.cmp.sameSize ? "" : ", INNY ROZMIAR");
            std::cout << "\n";
        }
    }
    if (failures > 20) std::cout << "... i " << (failures - 20) << " kolejnych bledow\n";

    std::cout << "\n" << cases.size() << " przypadkow, " << failures << " bledow, czas "
              << std::fixed << std::setprecision(1) << sw.wallMs() / 1000.0 << " s\n";
    if (cases.empty()) std::cout << "Uwaga: brak wariantow do sprawdzenia (poza V0)\n";
    return failures ? 3 : 0;
}

int main(int argc, char** argv)
{
    Options o;
    if (argc > 1) {
        if (!parseArgs(argc, argv, o)) return 1;
    } else {
        o = interactiveMenu();
    }

    if (o.verify) {
        bool variantGiven = false, maskGiven = false, kernelGiven = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "-v") variantGiven = true;
            if (std::string(argv[i]) == "-m") maskGiven    = true;
            if (std::string(argv[i]) == "-k") kernelGiven  = true;
        }
        return runVerify(o, variantGiven, maskGiven, kernelGiven);
    }

    const VariantInfo* v = findVariant(o.variant);
    if (!v) {
        std::cerr << "Nieznany wariant '" << o.variant << "'. Dostepne:\n";
        printVariants();
        return 1;
    }
    if (!v->available) {
        std::cerr << "Wariant " << v->name << " niedostepny: " << v->unavailableReason << "\n";
        return 1;
    }

    const Mask* mask = findMask(o.mask);
    if (!mask) {
        std::cout << "Niepoprawna maska '" << o.mask << "' – uzywam "
                  << availableMasks()[0].name << "\n";
        mask = &availableMasks()[0];
    }

    try {
        return o.benchmark ? runBenchmark(o, *v, *mask) : runSingle(o, *v, *mask);
    } catch (const std::exception& e) {
        std::cerr << "Blad: " << e.what() << "\n";
        return 1;
    }
}
