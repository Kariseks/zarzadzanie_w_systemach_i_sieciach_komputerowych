/**
 * @file engine.cpp
 * @brief Implementacja warstwy pomiarowej edge_gui (patrz engine.h).
 */
#include "engine.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QRegularExpression>
#include <QLocale>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <set>
#include <thread>

#include "topology.h"

#include "compare.h"
#include "gpu/gpu_opencl.h"
#include "image.h"
#include "masks.h"
#include "measure.h"
#include "synthetic.h"

// ============================================================================
//  Wersje W1–W5
// ============================================================================

QVector<Wersja> wszystkieWersje() { return { Wersja::W1, Wersja::W2, Wersja::W3, Wersja::W4, Wersja::W5 }; }

VariantId idWariantu(Wersja w)
{
    switch (w) {
    case Wersja::W1: return VariantId::W1;
    case Wersja::W2: return VariantId::W2;
    case Wersja::W3: return VariantId::W3;
    case Wersja::W4: return VariantId::W4;
    case Wersja::W5: return VariantId::W5;
    }
    return VariantId::W1;
}

QString nazwaWersji(Wersja w) { return QStringLiteral("W%1").arg(static_cast<int>(w) + 1); }

QString opisWersji(Wersja w)
{
    switch (w) {
    case Wersja::W1: return QStringLiteral("sekwencyjna");
    case Wersja::W2: return QStringLiteral("algorytm i pamięć");
    case Wersja::W3: return QStringLiteral("SIMD (AVX2)");
    case Wersja::W4: return QStringLiteral("wątki CPU");
    case Wersja::W5: return QStringLiteral("GPU (OpenCL)");
    }
    return {};
}

QString pomocWersji(Wersja w)
{
    switch (w) {
    case Wersja::W1:
        return QStringLiteral("<b>W1 – wersja sekwencyjna (punkt odniesienia)</b><br>"
                              "Splot 2D liczony wprost w jednym wątku: przy każdym odczycie współrzędne są obcinane "
                              "funkcją <code>std::clamp</code>, maska jest odczytywana z tablicy w czasie wykonania, "
                              "bufory są przydzielane przy każdym wywołaniu. Z W1 porównywane są wszystkie pozostałe wersje.");
    case Wersja::W2:
        return QStringLiteral("<b>W2 – algorytm i pamięć (1 rdzeń)</b><br>"
                              "Brzegi obrazu poza główną pętlą, maska jako stała czasu kompilacji, "
                              "<b>splot rozdzielny</b> (4k zamiast 2k² operacji na piksel), bufory <code>int16</code> "
                              "wyrównane do 64 B i używane wielokrotnie. Bez instrukcji wektorowych i bez wątków.");
    case Wersja::W3:
        return QStringLiteral("<b>W3 – wektoryzacja SIMD (1 rdzeń)</b><br>"
                              "Ten sam algorytm co W1, ale instrukcje AVX2 liczą <b>8 pikseli naraz</b>. "
                              "Kolumny brzegowe liczy kod W1. Teoretyczna granica przyspieszenia: 8×.");
    case Wersja::W4:
        return QStringLiteral("<b>W4 – wątki na rdzeniach CPU</b><br>"
                              "Jądro W1 bez zmian, obraz dzielony na fragmenty wierszy pomiędzy N wątków. "
                              "Własna <b>pula wątków</b> i <b>scheduler</b> (static, cyclic, dynamic, guided). "
                              "Dwa etapy rozdzielone barierą: splot z lokalnym maksimum, potem skalowanie.");
    case Wersja::W5:
        return QStringLiteral("<b>W5 – karta graficzna (OpenCL)</b><br>"
                              "Algorytm W1 jako kernel OpenCL: jeden wątek GPU liczy jeden lub kilka pikseli. "
                              "Czas obejmuje przesłanie obrazu na kartę i wyniku z powrotem. "
                              "Inicjalizacja OpenCL odbywa się raz, przed pomiarem.");
    }
    return {};
}

QColor kolorWersji(Wersja w)
{
    switch (w) {
    case Wersja::W1: return QColor("#595959");
    case Wersja::W2: return QColor("#0072B2");
    case Wersja::W3: return QColor("#009E73");
    case Wersja::W4: return QColor("#D55E00");
    case Wersja::W5: return QColor("#CC79A7");
    }
    return Qt::gray;
}

bool wersjaDostepna(Wersja w, QString* powod)
{
    const VariantInfo& vi = variantInfo(idWariantu(w));
    bool ok = vi.available;
    QString p = vi.unavailableReason ? QString::fromUtf8(vi.unavailableReason) : QString();
    if (ok && w == Wersja::W5 && gpuDevices().empty()) {
        ok = false;
        p = QStringLiteral("brak urządzenia OpenCL");
    }
    if (powod) *powod = p;
    return ok;
}

// ============================================================================
//  Narzędzia
// ============================================================================

Statystyka Statystyka::z(std::vector<double> v)
{
    Statystyka s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    s.mediana = (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    s.min = v.front();
    s.max = v.back();
    if (n >= 2) {
        double suma = 0, kw = 0;
        for (double x : v) suma += x;
        const double sr = suma / n;
        for (double x : v) kw += (x - sr) * (x - sr);
        s.cv = sr > 0 ? std::sqrt(kw / (n - 1)) / sr : 0;
    }
    return s;
}

QImage doQImage(const std::vector<std::uint8_t>& dane, int szer, int wys)
{
    QImage img(szer, wys, QImage::Format_Grayscale8);
    for (int y = 0; y < wys; ++y)
        std::memcpy(img.scanLine(y), dane.data() + static_cast<size_t>(y) * szer, static_cast<size_t>(szer));
    return img;
}

QString katalogRepo()
{
    // Szukane w górę od katalogu programu (np. implementacja/build-qtcreator/ → ../../),
    // więc cały folder projektu można przenieść albo skopiować na inny komputer.
    static const QString repo = [] {
        auto pasuje = [](const QDir& d) { return d.exists(QStringLiteral("obrazy")) && d.exists(QStringLiteral("implementacja")); };
        for (const QString& start : { QCoreApplication::applicationDirPath(), QDir::currentPath() }) {
            QDir d(start);
            for (int i = 0; i < 6; ++i) {
                if (pasuje(d)) return d.absolutePath();
                if (!d.cdUp()) break;
            }
        }
        return QDir::currentPath();
    }();
    return repo;
}

QVector<ZrodloObrazu> obrazyTestowe()
{
    const QString r = katalogRepo();
    const QVector<ZrodloObrazu> kandydaci = {
        { r + "/obrazy/kodim08.png", "Kodak 08 – 768×512 (0,4 Mpix)" },
        { r + "/obrazy/kodim23.png", "Kodak 23 – 768×512 (0,4 Mpix)" },
        { r + "/obrazy/skalowane/1920x1080.png", "HD – 1920×1080 (2,1 Mpix)" },
        { r + "/obrazy/skalowane/3840x2160.jpg", "4K – 3840×2160 (8,3 Mpix), JPG" },
        { r + "/obrazy/skalowane/7680x4320.jpg", "8K – 7680×4320 (33 Mpix), JPG" },
        { r + "/obrazy/skalowane/10240x5760.png", "10240×5760 (59 Mpix)" },
        { r + "/obrazy/jakosc/szum_30.png", "Figury z szumem σ = 30 – 768×512" },
        { "synthetic:1920x1080", "Syntetyczny HD (generowany)" },
        { "synthetic:3840x2160", "Syntetyczny 4K (generowany)" },
        { "synthetic:7680x4320", "Syntetyczny 8K (generowany)" },
    };
    QVector<ZrodloObrazu> out;
    for (const auto& k : kandydaci)
        if (k.syntetyczny() || QFileInfo::exists(k.sciezka)) out.push_back(k);
    return out;
}

int znaki(const QWidget* w, double n) { return static_cast<int>(n * w->fontMetrics().averageCharWidth()); }
int wiersze(const QWidget* w, double n) { return static_cast<int>(n * w->fontMetrics().height()); }

QString liczba(double v, int miejsca)
{
    if (miejsca < 0) {
        const double a = std::abs(v);
        miejsca = a >= 100 ? 0 : a >= 10 ? 1 : a >= 1 ? 2 : 3;
    }
    return QLocale(QLocale::Polish).toString(v, 'f', miejsca);
}

QString tekstCzasu(double ms)
{
    if (ms >= 1000) return liczba(ms / 1000.0, 2) + QStringLiteral(" s");
    return liczba(ms) + QStringLiteral(" ms");
}

QString regulatorCzestotliwosci()
{
    QFile f("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(f.readAll()).trimmed();
}

namespace {
/** Odczyt (zajęty, wszystkie) z pierwszej linii /proc/stat. */
bool czasyCpu(unsigned long long& zajety, unsigned long long& razem)
{
    std::ifstream f("/proc/stat");
    std::string cpu;
    unsigned long long u, n, s, idle, io, irq, sirq, steal;
    if (!(f >> cpu >> u >> n >> s >> idle >> io >> irq >> sirq >> steal)) return false;
    zajety = u + n + s + irq + sirq + steal;
    razem = zajety + idle + io;
    return true;
}
} // namespace

double zajetoscCpu()
{
    unsigned long long z0, r0, z1, r1;
    if (!czasyCpu(z0, r0)) return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!czasyCpu(z1, r1) || r1 == r0) return -1;
    return 100.0 * static_cast<double>(z1 - z0) / static_cast<double>(r1 - r0);
}

namespace {
QString czytaj(const QString& plik)
{
    QFile f(plik);
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}
} // namespace

WarunkiPomiaru odczytajWarunki(bool mierzZajetosc)
{
    WarunkiPomiaru w;
    w.regulator = regulatorCzestotliwosci();
    w.preferencja = czytaj("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference");
    w.profil = czytaj("/sys/firmware/acpi/platform_profile");
    // Zasilacz: źródła typu „Mains”; baterie urządzeń (mysz, klawiatura – scope „Device”) są pomijane
    const QDir ps("/sys/class/power_supply");
    bool jestBateria = false;
    for (const QString& e : ps.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString typ = czytaj(ps.filePath(e) + "/type");
        if (typ == QLatin1String("Mains")) w.zasilacz = std::max(w.zasilacz, czytaj(ps.filePath(e) + "/online").toInt());
        if (typ == QLatin1String("Battery") && czytaj(ps.filePath(e) + "/scope") != QLatin1String("Device")) jestBateria = true;
    }
    if (!jestBateria) w.zasilacz = -1;          // komputer stacjonarny – bez ostrzeżeń o baterii
    else if (w.zasilacz < 0) w.zasilacz = 0;    // jest bateria, brak informacji o zasilaczu
    if (mierzZajetosc) w.zajetosc = zajetoscCpu();
    return w;
}

QString WarunkiPomiaru::opis() const
{
    QStringList t;
    if (zasilacz == 1) t << QStringLiteral("zasilacz");
    else if (zasilacz == 0) t << QStringLiteral("BATERIA");
    if (!profil.isEmpty()) t << QStringLiteral("profil %1").arg(profil);
    if (!regulator.isEmpty()) t << regulator;
    if (!preferencja.isEmpty() && preferencja != regulator) t << QStringLiteral("EPP %1").arg(preferencja);
    if (zajetosc >= 0) t << QStringLiteral("CPU %1%").arg(liczba(zajetosc, 0));
    return t.join(QStringLiteral(", "));
}

QStringList WarunkiPomiaru::ostrzezenia() const
{
    QStringList w;
    if (zasilacz == 0) w << QStringLiteral("laptop na baterii – podłącz zasilacz (na baterii procesor ma niższy limit mocy)");
    if (!profil.isEmpty() && profil != QLatin1String("performance"))
        w << QStringLiteral("profil zasilania „%1” – ustaw „performance” (Ustawienia → Zasilanie)").arg(profil);
    if (!regulator.isEmpty() && regulator != QLatin1String("performance") && preferencja != QLatin1String("performance"))
        w << QStringLiteral("regulator częstotliwości „%1” – czasy mogą się wahać").arg(regulator);
    if (zajetosc > 5) w << QStringLiteral("inne programy zajmują %1% procesora").arg(liczba(zajetosc, 0));
    return w;
}

QString ostrzezenieOWarunkach() { return odczytajWarunki(false).ostrzezenia().join("; "); }

Topologia topologia()
{
    Topologia t;
    const auto& cpu = cpuTopology();
    if (cpu.empty()) {
        t.logiczne = t.fizyczne = t.rdzeniWBloku = std::max(1, QThread::idealThreadCount());
        return t;
    }
    std::map<int, std::set<int>> bloki;   // l3 → rdzenie
    std::set<int> rdzenie;
    for (const auto& c : cpu) { bloki[c.l3].insert(c.core); rdzenie.insert(c.core); }
    t.logiczne = static_cast<int>(cpu.size());
    t.fizyczne = static_cast<int>(rdzenie.size());
    t.bloki = static_cast<int>(bloki.size());
    for (const auto& [_, r] : bloki) t.rdzeniWBloku = std::max(t.rdzeniWBloku, static_cast<int>(r.size()));
    return t;
}

// ============================================================================
//  Pamięć podręczna wyników W1 (wzorzec poprawności i czas bazowy)
// ============================================================================

namespace {
struct WpisW1 {
    std::vector<std::uint8_t> wynik;
    double medianaMs = 0;    // 0 = jeszcze nie mierzono czasu W1
};
std::mutex                    g_mutexW1;
std::map<std::string, WpisW1> g_cacheW1;

std::string kluczW1(const Zlecenie& z)
{
    std::ostringstream k;
    k << z.zrodlo.sciezka.toStdString() << '|' << z.maska.toStdString() << '|'
      << static_cast<int>(z.cfg.norm) << '|' << static_cast<int>(z.cfg.scaling);
    return k.str();
}
} // namespace

// ============================================================================
//  Pomiar
// ============================================================================

Pomiar zmierz(const Zlecenie& z, const std::atomic<bool>& przerwij, const Postep& postep)
{
    Pomiar p;
    p.zlecenie = z;
    const std::string src = z.zrodlo.sciezka.toStdString();
    const Mask* mask = findMask(z.maska.toStdString());
    if (!mask) { p.blad = QStringLiteral("nieznana maska %1").arg(z.maska); return p; }

    QString powod;
    if (!wersjaDostepna(z.wersja, &powod)) {
        p.blad = QStringLiteral("%1 jest niedostępna: %2").arg(nazwaWersji(z.wersja), powod);
        return p;
    }

    GrayImage img;
    if (!loadOrGenerate(src, img)) { p.blad = QStringLiteral("nie można wczytać obrazu %1").arg(z.zrodlo.sciezka); return p; }
    p.szer = img.width;
    p.wys = img.height;
    p.wejscie = doQImage(img.data, img.width, img.height);

    const int kroki = z.rozgrzewka + z.powtorzenia + (z.porownajZW1 ? 1 : 0);
    int krok = 0;
    auto zglos = [&](const QString& opis) { if (postep) postep(++krok, kroki, opis); };

    try {
        VariantRunner runner;
        const VariantId id = idWariantu(z.wersja);
        const QString tmp = QDir::temp().filePath(QStringLiteral("edge_gui_%1.jpg").arg(QCoreApplication::applicationPid()));

        for (int i = 0; i < z.rozgrzewka; ++i) {
            if (przerwij) { p.blad = QStringLiteral("przerwano"); return p; }
            RunResult r = runner.run(id, img, *mask, z.cfg);
            runner.recycle(r);
            zglos(QStringLiteral("rozgrzewka %1/%2").arg(i + 1).arg(z.rozgrzewka));
        }

        std::vector<double> tW, tA, tZ, tC, imb;
        std::map<std::string, std::vector<double>> etapy;
        RunResult ostatni;
        for (int i = 0; i < z.powtorzenia; ++i) {
            if (przerwij) { p.blad = QStringLiteral("przerwano"); return p; }
            Stopwatch caly;
            GrayImage swiezy;
            const GrayImage* we = &img;
            double wczyt = 0, zap = 0;
            if (z.zWczytaniemIZapisem) {
                Stopwatch t;
                if (!loadOrGenerate(src, swiezy)) throw std::runtime_error("nie można wczytać obrazu");
                wczyt = t.wallMs();
                we = &swiezy;
            }
            RunResult r = runner.run(id, *we, *mask, z.cfg);
            if (z.zWczytaniemIZapisem) {
                Stopwatch t;
                saveGrayJpg(tmp.toStdString(), r.output, img.width, img.height);
                zap = t.wallMs();
            }
            const double calosc = caly.wallMs();
            tW.push_back(wczyt);
            tA.push_back(r.wallMs);
            tZ.push_back(zap);
            tC.push_back(z.zWczytaniemIZapisem ? calosc : r.wallMs);
            for (const auto& [k, v] : r.stagesMs) etapy[k].push_back(v);
            if (isThreadPoolVariant(id)) imb.push_back(r.imbalanceMs);
            p.watki = r.threadsUsed;
            p.watkiGpu = r.gpuWorkItems;
            if (i + 1 < z.powtorzenia) runner.recycle(r);
            else ostatni = std::move(r);
            zglos(QStringLiteral("powtórzenie %1/%2").arg(i + 1).arg(z.powtorzenia));
        }
        QFile::remove(tmp);

        p.wczytanie = Statystyka::z(tW);
        p.algorytm = Statystyka::z(tA);
        p.zapis = Statystyka::z(tZ);
        p.calosc = Statystyka::z(tC);
        for (const auto& [k, v] : etapy) p.etapy[k] = Statystyka::z(v).mediana;
        p.nierownowagaMs = Statystyka::z(imb).mediana;
        for (double t : tA) p.czasyAlgorytmu.push_back(t);
        if (!isGpuVariant(id)) {
            const double conv = p.etapy.count("conv") ? p.etapy["conv"] : 0;
            const double norm = p.etapy.count("norm") ? p.etapy["norm"] : 0;
            p.pozostaleMs = std::max(0.0, p.algorytm.mediana - conv - norm);
        } else {
            double s = 0;
            for (const char* k : { "upload", "kernel", "download" }) if (p.etapy.count(k)) s += p.etapy[k];
            p.pozostaleMs = std::max(0.0, p.algorytm.mediana - s);
        }
        p.wynik = doQImage(ostatni.output, img.width, img.height);

        // Wzorzec W1: wynik do porównania i (gdy znany) czas bazowy
        const std::string klucz = kluczW1(z);
        if (z.wersja == Wersja::W1) {
            std::lock_guard<std::mutex> l(g_mutexW1);
            WpisW1& e = g_cacheW1[klucz];
            e.wynik = ostatni.output;
            e.medianaMs = p.algorytm.mediana;
        }
        if (z.porownajZW1) {
            std::vector<std::uint8_t> ref;
            {
                std::lock_guard<std::mutex> l(g_mutexW1);
                auto it = g_cacheW1.find(klucz);
                if (it != g_cacheW1.end()) ref = it->second.wynik;
            }
            if (ref.empty()) {
                RunConfig c1 = z.cfg;
                RunResult r1 = runner.run(VariantId::W1, img, *mask, c1);
                ref = r1.output;
                std::lock_guard<std::mutex> l(g_mutexW1);
                g_cacheW1[klucz].wynik = ref;
            }
            const Comparison c = compareImages(ref, ostatni.output);
            p.porownano = true;
            p.identyczny = c.identical;
            p.maksRoznica = c.maxAbsDiff;
            zglos(QStringLiteral("porównanie z W1"));
        }
        {
            std::lock_guard<std::mutex> l(g_mutexW1);
            auto it = g_cacheW1.find(klucz);
            if (it != g_cacheW1.end()) p.czasW1Ms = it->second.medianaMs;
        }
        p.ok = true;
        p.kiedy = QDateTime::currentDateTime();
    } catch (const std::exception& e) {
        p.blad = QString::fromUtf8(e.what());
    }
    return p;
}
