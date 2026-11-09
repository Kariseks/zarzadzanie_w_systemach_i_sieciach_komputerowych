// ============================================================================
//  edge_bench – konsolowa aplikacja Qt do pomiarow (bez okna)
//
//  Wykonuje eksperyment z pliku JSON i/lub flag, zapisuje surowe wyniki
//  (results.csv, correctness.csv, meta.json).  Wywolywana przez skrypty
//  Pythona w analiza/.
//
//  Kontrakt:
//    stdout – wylacznie linie JSON ze zdarzeniami (progress, warning, done,
//             oraz odpowiedzi --list-variants / --list-devices / --verify)
//    stderr – logi dla czlowieka
//    kody wyjscia: 0 OK, 1 zle argumenty, 2 wariant niedostepny przy --strict,
//                  3 blad poprawnosci, 4 blad wejscia-wyjscia / OpenCL
//    Ctrl+C / SIGTERM – przerwanie po biezacym powtorzeniu, zapis tego, co
//             zmierzone, "complete": false w meta.json, kod 130
// ============================================================================
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSysInfo>
#include <QTextStream>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <thread>

#include "gpu/gpu_opencl.h"
#include "harness.h"
#include "topology.h"
#include "measure.h"
#include "variants.h"
#include "verify.h"

#ifndef EDGE_BUILD_TYPE
#define EDGE_BUILD_TYPE "unknown"
#endif
#ifndef EDGE_CXX_FLAGS
#define EDGE_CXX_FLAGS ""
#endif
#ifndef EDGE_SOURCE_DIR
#define EDGE_SOURCE_DIR "."
#endif

namespace {

enum ExitCode { ExitOk = 0, ExitArgs = 1, ExitUnavailable = 2, ExitCorrectness = 3, ExitIo = 4, ExitInterrupted = 130 };

std::atomic<bool> g_stop{ false };
extern "C" void onSignal(int) { g_stop.store(true); }

// --- wyjscie -----------------------------------------------------------------
void emitJson(const QJsonObject& o)
{
    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact);
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void emitEvent(const QString& type, QJsonObject o = {})
{
    o.insert("event", type);
    emitJson(o);
}

void logErr(const QString& s) { std::cerr << s.toStdString() << std::endl; }

[[noreturn]] void failArgs(const QString& msg)
{
    logErr("edge_bench: " + msg);
    emitEvent("error", { { "code", ExitArgs }, { "message", msg } });
    std::exit(ExitArgs);
}

// --- informacje o systemie -------------------------------------------------
QString readFirstLine(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readLine()).trimmed();
}

// /proc/cpuinfo ma rozmiar 0, wiec atEnd() nie dziala – czytamy readAll()
QStringList cpuinfoLines()
{
    QFile f("/proc/cpuinfo");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll()).split('\n');
}

QString cpuModel()
{
    for (const auto& line : cpuinfoLines())
        if (line.startsWith("model name")) return line.section(':', 1).trimmed();
    return QSysInfo::currentCpuArchitecture();
}

QString cpuFlagsOf(const QStringList& wanted)
{
    QStringList found;
    for (const auto& line : cpuinfoLines()) {
        if (!line.startsWith("flags")) continue;
        const QStringList flags = line.section(':', 1).split(' ', Qt::SkipEmptyParts);
        for (const auto& w : wanted)
            if (flags.contains(w)) found << w;
        break;
    }
    return found.join(',');
}

QString governor()
{
    const QString g = readFirstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    return g.isEmpty() ? "unknown" : g;
}

QString gitDescribe()
{
    QProcess p;
    p.start("git", { "-C", EDGE_SOURCE_DIR, "describe", "--always", "--dirty", "--tags" });
    if (!p.waitForFinished(3000) || p.exitCode() != 0) return "unknown";
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

QString compilerString()
{
#if defined(__clang__)
    return QString("clang %1.%2.%3").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
#elif defined(__GNUC__)
    return QString("gcc %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return QString("msvc %1").arg(_MSC_VER);
#else
    return "unknown";
#endif
}

int hwThreads()
{
    const int n = static_cast<int>(std::thread::hardware_concurrency());
    return n > 0 ? n : 1;
}

QJsonObject systemInfo()
{
    return {
        { "cpu_model", cpuModel() },
        { "logical_cpus", hwThreads() },
        { "simd", cpuFlagsOf({ "sse4_2", "avx", "avx2", "fma", "avx512f", "avx512bw" }) },
        { "governor", governor() },
        { "os", QSysInfo::prettyProductName() },
        { "kernel", QSysInfo::kernelVersion() },
        { "hostname", QSysInfo::machineHostName() },
        { "compiler", compilerString() },
        { "cxx_flags", QString(EDGE_CXX_FLAGS).simplified() },
        { "build_type", QString(EDGE_BUILD_TYPE) },
        { "qt_version", QString(qVersion()) },
        { "topology", QString::fromStdString(topologySummary()) },
        { "opencl_compiled", gpuCompiledIn() },
    };
}

QJsonArray variantsJson()
{
    QJsonArray arr;
    for (const auto& v : allVariants())
        arr.append(QJsonObject{
            { "id", v.name }, { "description", v.description },
            { "multithreaded", v.multithreaded }, { "available", v.available },
            { "unavailable_reason", v.unavailableReason },
            { "tolerance", v.maxAllowedDiff == kApproximate ? QJsonValue("approx") : QJsonValue(v.maxAllowedDiff) },
        });
    return arr;
}

QString sha256OfFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

// --- parsowanie --------------------------------------------------------------
QStringList splitList(const QString& s)
{
    QStringList out;
    for (const auto& part : s.split(',', Qt::SkipEmptyParts)) out << part.trimmed();
    return out;
}

std::vector<std::string> toStd(const QStringList& l)
{
    std::vector<std::string> v;
    for (const auto& s : l) v.push_back(s.toStdString());
    return v;
}

// Pole JSON: lista albo napis "a,b,c" / "1-24"
QString jsonListAsText(const QJsonValue& v)
{
    if (v.isArray()) {
        QStringList parts;
        for (const auto& x : v.toArray())
            parts << (x.isDouble() ? QString::number(x.toInt()) : x.toString());
        return parts.join(',');
    }
    if (v.isDouble()) return QString::number(v.toInt());
    return v.toString();
}

std::vector<int> parseInts(const QString& text, const char* what)
{
    std::vector<int> out;
    if (!parseIntList(text.toStdString(), out, hwThreads()))
        failArgs(QString("zla lista %1: '%2' (np. 1-24, 1,2,4, max)").arg(what, text));
    return out;
}

std::vector<Schedule> parseSchedules(const QString& text)
{
    std::vector<Schedule> out;
    for (const auto& s : splitList(text)) {
        if (s == "all") return { Schedule::Static, Schedule::Cyclic, Schedule::Dynamic, Schedule::Guided };
        Schedule sch;
        if (!parseSchedule(s.toStdString(), sch)) failArgs("nieznany harmonogram: " + s);
        out.push_back(sch);
    }
    if (out.empty()) failArgs("pusta lista harmonogramow");
    return out;
}

void applyField(const QString& key, const QString& text, ExperimentSpec& spec)
{
    if      (key == "variants")  spec.variants  = toStd(splitList(text));
    else if (key == "images")    spec.images    = toStd(splitList(text));
    else if (key == "masks")     spec.masks     = toStd(splitList(text));
    else if (key == "threads")   spec.threads   = parseInts(text, "watkow");
    else if (key == "chunks")    spec.chunks    = parseInts(text, "fragmentow");
    else if (key == "schedules") spec.schedules = parseSchedules(text);
    else if (key == "kernels")   spec.kernels   = toStd(splitList(text));
    else if (key == "mechs") {
        spec.mechs.clear();
        for (const auto& n : splitList(text)) {
            ThreadMech v; if (!parseThreadMech(n.toStdString(), v)) failArgs("nieznany mechanizm: " + n);
            spec.mechs.push_back(v);
        }
        if (spec.mechs.empty()) failArgs("pusta lista mechanizmow");
    }
    else if (key == "reduces") {
        spec.reduces.clear();
        for (const auto& n : splitList(text)) {
            if (n == "all") { spec.reduces = { Reduce::Local, Reduce::Atomic, Reduce::Mutex, Reduce::PixelPadded, Reduce::PixelUnpadded }; break; }
            Reduce v; if (!parseReduce(n.toStdString(), v)) failArgs("nieznana redukcja: " + n);
            spec.reduces.push_back(v);
        }
        if (spec.reduces.empty()) failArgs("pusta lista redukcji");
    }
    else if (key == "affinities") {
        spec.affinities.clear();
        for (const auto& n : splitList(text)) {
            if (n == "all") { spec.affinities = { Affinity::None, Affinity::Compact, Affinity::Spread, Affinity::NoSmt }; break; }
            Affinity v; if (!parseAffinity(n.toStdString(), v)) failArgs("nieznane przypinanie: " + n);
            spec.affinities.push_back(v);
        }
        if (spec.affinities.empty()) failArgs("pusta lista przypinania");
    }
    else if (key == "gpu_workgroups") {
        spec.gpuWorkgroups.clear();
        for (const auto& n : splitList(text)) {
            const QStringList p = n.toLower().split('x');
            bool ok1 = false, ok2 = false;
            const int a = p.value(0).toInt(&ok1), b = p.value(1).toInt(&ok2);
            if (p.size() != 2 || !ok1 || !ok2 || a < 1 || b < 1 || a * b > 1024 || (a * b & (a * b - 1)))
                failArgs("zla grupa robocza GPU: " + n + " (np. 16x16; iloczyn = potega 2, maks. 1024)");
            spec.gpuWorkgroups.push_back({ a, b });
        }
        if (spec.gpuWorkgroups.empty()) failArgs("pusta lista grup roboczych");
    }
    else if (key == "gpu_ppws") {
        spec.gpuPpws = parseInts(text, "pikseli na work-item");
        for (int v : spec.gpuPpws) if (v < 1) failArgs("liczba pikseli na work-item musi byc >= 1");
    }
    else if (key == "gpu_precisions") {
        spec.gpuFp32.clear();
        for (const auto& n : splitList(text)) {
            if (n == "auto") spec.gpuFp32.push_back(false);
            else if (n == "fp32") spec.gpuFp32.push_back(true);
            else failArgs("nieznana precyzja GPU: " + n + " (auto | fp32)");
        }
        if (spec.gpuFp32.empty()) failArgs("pusta lista precyzji GPU");
    }
    else if (key == "norms") {
        spec.norms.clear();
        for (const auto& n : splitList(text)) {
            if (n == "all") { spec.norms = { Norm::L2, Norm::L1, Norm::Linf }; break; }
            Norm v; if (!parseNorm(n.toStdString(), v)) failArgs("nieznana norma: " + n);
            spec.norms.push_back(v);
        }
        if (spec.norms.empty()) failArgs("pusta lista norm");
    }
    else if (key == "scalings") {
        spec.scalings.clear();
        for (const auto& n : splitList(text)) {
            if (n == "all") { spec.scalings = { Scaling::GlobalMax, Scaling::Fixed, Scaling::Clamp }; break; }
            Scaling v; if (!parseScaling(n.toStdString(), v)) failArgs("nieznane skalowanie: " + n);
            spec.scalings.push_back(v);
        }
        if (spec.scalings.empty()) failArgs("pusta lista skalowan");
    }
}

void loadExperimentFile(const QString& path, ExperimentSpec& spec, QJsonObject& raw)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) failArgs("nie mozna otworzyc pliku eksperymentu: " + path);
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        failArgs(QString("bledny JSON w %1: %2 (pozycja %3)").arg(path, err.errorString()).arg(err.offset));
    raw = doc.object();

    static const QStringList known = { "name", "description", "variants", "kernels", "images", "masks",
                                       "norms", "scalings", "threads", "mechs", "reduces", "affinities",
                                       "gpu_precisions", "gpu_workgroups", "gpu_ppws", "include_io",
                                       "schedules", "chunks", "warmup", "repeats", "shuffle", "save_images", "strict" };
    for (const auto& k : raw.keys())
        if (!known.contains(k)) failArgs(QString("nieznane pole '%1' w %2").arg(k, path));

    spec.name = raw.value("name").toString(QFileInfo(path).completeBaseName()).toStdString();
    for (const char* key : { "variants", "kernels", "images", "masks", "norms", "scalings",
                             "threads", "schedules", "chunks", "mechs", "reduces", "affinities",
                             "gpu_precisions", "gpu_workgroups", "gpu_ppws" })
        if (raw.contains(key)) applyField(key, jsonListAsText(raw.value(key)), spec);
    if (raw.contains("warmup"))      spec.warmup      = raw.value("warmup").toInt(spec.warmup);
    if (raw.contains("repeats"))     spec.repeats     = raw.value("repeats").toInt(spec.repeats);
    if (raw.contains("shuffle"))     spec.shuffleSeed = static_cast<std::uint32_t>(raw.value("shuffle").toInt(0));
    if (raw.contains("save_images")) spec.saveImages  = raw.value("save_images").toBool();
    if (raw.contains("strict"))      spec.strict      = raw.value("strict").toBool();
    if (raw.contains("include_io"))  spec.includeIo   = raw.value("include_io").toBool();
}

QJsonObject specJson(const ExperimentSpec& s)
{
    auto arr = [](const auto& v) { QJsonArray a; for (const auto& x : v) a.append(QString::fromStdString(x)); return a; };
    QJsonArray th, ch, sc;
    for (int t : s.threads) th.append(t);
    for (int c : s.chunks)  ch.append(c);
    for (auto x : s.schedules) sc.append(toString(x));
    QJsonArray no, sl, me, re, af, gp;
    for (auto x : s.mechs)      me.append(toString(x));
    for (auto x : s.reduces)    re.append(toString(x));
    for (auto x : s.affinities) af.append(toString(x));
    for (bool x : s.gpuFp32)    gp.append(x ? "fp32" : "auto");
    QJsonArray gw, gpp;
    for (const auto& x : s.gpuWorkgroups) gw.append(QString("%1x%2").arg(x.first).arg(x.second));
    for (int x : s.gpuPpws) gpp.append(x);
    for (auto x : s.norms)    no.append(toString(x));
    for (auto x : s.scalings) sl.append(toString(x));
    return { { "name", QString::fromStdString(s.name) }, { "variants", arr(s.variants) },
             { "kernels", arr(s.kernels) }, { "images", arr(s.images) }, { "masks", arr(s.masks) },
             { "norms", no }, { "scalings", sl }, { "threads", th },
             { "mechs", me }, { "reduces", re }, { "affinities", af }, { "gpu_precisions", gp },
             { "gpu_workgroups", gw }, { "gpu_ppws", gpp },
             { "schedules", sc }, { "chunks", ch }, { "warmup", s.warmup }, { "repeats", s.repeats },
             { "shuffle", static_cast<qint64>(s.shuffleSeed) }, { "save_images", s.saveImages },
             { "strict", s.strict }, { "include_io", s.includeIo } };
}

bool isDebugBuild()
{
    const QString bt = QString(EDGE_BUILD_TYPE).toLower();
    return bt == "debug" || bt.isEmpty() || bt == "unknown";
}

// --- tryby -------------------------------------------------------------------
int runVerifyMode(const ExperimentSpec& spec, bool variantsGiven, bool masksGiven, bool imagesGiven,
                  bool kernelsGiven, bool normsGiven, bool scalingsGiven)
{
    VerifyOptions vo;
    if (kernelsGiven)  vo.kernels  = spec.kernels;
    if (normsGiven)    vo.norms    = spec.norms;
    if (scalingsGiven) vo.scalings = spec.scalings;
    if (variantsGiven) vo.variants = spec.variants;
    if (masksGiven)    vo.masks    = spec.masks;
    if (imagesGiven)   vo.images   = spec.images;

    std::vector<VerifyCase> cases;
    try {
        cases = runVerification(vo, [](size_t done, size_t total) {
            if (done % 100 == 0 || done == total)
                emitEvent("progress", { { "done", static_cast<qint64>(done) }, { "total", static_cast<qint64>(total) } });
        });
    } catch (const std::exception& e) {
        logErr(QString("edge_bench: ") + e.what());
        emitEvent("error", { { "code", ExitArgs }, { "message", e.what() } });
        return ExitArgs;
    }

    QJsonArray failures;
    for (const auto& c : cases)
        if (!c.pass && failures.size() < 50)
            failures.append(QJsonObject{
                { "variant", QString::fromStdString(c.variant) }, { "kernel", QString::fromStdString(c.kernel) },
                { "image", QString::fromStdString(c.image) },
                { "norm", toString(c.cfg.norm) }, { "scaling", toString(c.cfg.scaling) },
                { "mask", QString::fromStdString(c.mask) }, { "threads", c.cfg.threads },
                { "schedule", toString(c.cfg.schedule) }, { "chunk", c.cfg.chunkRows },
                { "max_abs_diff", c.cmp.maxAbsDiff }, { "error", QString::fromStdString(c.error) } });
    size_t nFail = 0;
    for (const auto& c : cases) nFail += c.pass ? 0 : 1;

    emitEvent("verify", { { "cases", static_cast<qint64>(cases.size()) },
                          { "failures", static_cast<qint64>(nFail) }, { "failed_cases", failures } });
    logErr(QString("weryfikacja: %1 przypadkow, %2 bledow").arg(cases.size()).arg(nFail));
    return nFail ? ExitCorrectness : ExitOk;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("edge_bench");
    QCoreApplication::setApplicationVersion("0.1");

    QCommandLineParser p;
    p.setApplicationDescription(
        "Pomiary wariantow wykrywania krawedzi. Wyniki: results.csv, correctness.csv, meta.json.\n"
        "Listy: przecinki; liczby: zakresy 1-24 i 'max'. Obraz: plik albo synthetic:WxH[:ziarno].");
    p.addHelpOption();
    p.addVersionOption();

    const QCommandLineOption oExperiment("experiment", "Plik eksperymentu JSON.", "plik");
    const QCommandLineOption oOut("out", "Katalog wynikow (nie moze istniec, chyba ze --force).", "katalog");
    const QCommandLineOption oForce("force", "Nadpisz istniejacy katalog wynikow.");
    const QCommandLineOption oName("name", "Nazwa eksperymentu (domyslnie z nazwy pliku).", "nazwa");
    const QCommandLineOption oVariants("variants", "Warianty, np. V0,T albo all.", "lista");
    const QCommandLineOption oImages(QStringList{ "images", "image" }, "Obrazy (pliki / synthetic:WxH[:ziarno]).", "lista");
    const QCommandLineOption oSynthetic("synthetic", "Skrot: --synthetic WxH[:ziarno] = --image synthetic:WxH.", "WxH");
    const QCommandLineOption oMasks(QStringList{ "masks", "mask" }, "Maski: sobel3,sobel5,prewitt,scharr,roberts.", "lista");
    const QCommandLineOption oKernels("kernels", "Jadra dla wariantu T, np. V0,V4 albo all.", "lista");
    const QCommandLineOption oNorms("norms", "Normy gradientu: l2,l1,linf albo all.", "lista");
    const QCommandLineOption oScalings("scalings", "Skalowanie: global,fixed,clamp albo all.", "lista");
    const QCommandLineOption oMechs("mechs", "Mechanizm watkow T: pool,spawn.", "lista");
    const QCommandLineOption oReduces("reduces", "Redukcja maksimum T: local,atomic,mutex,px_pad,px_nopad albo all.", "lista");
    const QCommandLineOption oAffinities("affinities", "Przypinanie T: none,compact,spread,nosmt albo all.", "lista");
    const QCommandLineOption oGpuPrec("gpu-precisions", "Precyzja GPU: auto (dokladnie), fp32.", "lista");
    const QCommandLineOption oGpuWg("gpu-workgroups", "Grupy robocze kernela GPU/W5, np. 8x8,16x16,32x8.", "lista");
    const QCommandLineOption oGpuPpw("gpu-ppws", "Pikseli na work-item kernela GPU/W5, np. 1,2,4,8.", "lista");
    const QCommandLineOption oThreads("threads", "Liczby watkow, np. 1-24 albo 1,2,4,max.", "lista");
    const QCommandLineOption oSchedules("schedules", "static,cyclic,dynamic,guided albo all.", "lista");
    const QCommandLineOption oChunks("chunks", "Rozmiary fragmentu w wierszach, np. 1,16,256.", "lista");
    const QCommandLineOption oWarmup("warmup", "Przebiegi rozgrzewki (nie zapisywane).", "N");
    const QCommandLineOption oRepeats("repeats", "Zapisywane powtorzenia.", "N");
    const QCommandLineOption oShuffle("shuffle", "Tasowanie kolejnosci przypadkow (ziarno, 0 = brak).", "ziarno");
    const QCommandLineOption oSaveImages("save-images", "Zapisz obrazy wynikowe do <out>/images.");
    const QCommandLineOption oStrict("strict", "Niedostepny wariant = blad (kod 2).");
    const QCommandLineOption oAllowDebug("allow-debug", "Pozwol mierzyc w buildzie Debug.");
    const QCommandLineOption oListVariants("list-variants", "Lista wariantow (JSON) i koniec.");
    const QCommandLineOption oListDevices("list-devices", "Informacje o systemie i urzadzeniach (JSON) i koniec.");
    const QCommandLineOption oVerify("verify", "Tylko poprawnosc wzgledem V0 (bez pomiaru czasu).");

    p.addOptions({ oMechs, oReduces, oAffinities, oGpuPrec, oGpuWg, oGpuPpw });
    p.addOptions({ oExperiment, oOut, oForce, oName, oVariants, oKernels, oNorms, oScalings,
                   oImages, oSynthetic, oMasks, oThreads,
                   oSchedules, oChunks, oWarmup, oRepeats, oShuffle, oSaveImages, oStrict, oAllowDebug,
                   oListVariants, oListDevices, oVerify });

    if (!p.parse(app.arguments())) failArgs(p.errorText());
    if (p.isSet("help"))    { std::cout << p.helpText().toStdString(); return ExitOk; }
    if (p.isSet("version")) { std::cout << "edge_bench 0.1\n"; return ExitOk; }
    if (!p.positionalArguments().isEmpty()) failArgs("nieoczekiwane argumenty: " + p.positionalArguments().join(' '));

    if (p.isSet(oListVariants)) { emitJson({ { "variants", variantsJson() } }); return ExitOk; }
    if (p.isSet(oListDevices)) {
        QJsonArray ocl;
        for (const auto& g : gpuDevices())
            ocl.append(QJsonObject{ { "platform", QString::fromStdString(g.platform) },
                                    { "device", QString::fromStdString(g.device) },
                                    { "version", QString::fromStdString(g.version) },
                                    { "driver", QString::fromStdString(g.driver) },
                                    { "fp64", g.fp64 }, { "global_mem_mb", g.globalMemMB },
                                    { "compute_units", g.computeUnits } });
        emitJson({ { "system", systemInfo() }, { "opencl", ocl }, { "variants", variantsJson() } });
        return ExitOk;
    }

    // --- specyfikacja: plik + nadpisania z linii polecen ----------------------
    ExperimentSpec spec;
    spec.variants = { "V0", "T" };
    QJsonObject rawExperiment;
    if (p.isSet(oExperiment)) loadExperimentFile(p.value(oExperiment), spec, rawExperiment);

    if (p.isSet(oName))      spec.name = p.value(oName).toStdString();
    if (p.isSet(oVariants))  applyField("variants",  p.value(oVariants),  spec);
    if (p.isSet(oMasks))     applyField("masks",     p.value(oMasks),     spec);
    if (p.isSet(oKernels))   applyField("kernels",   p.value(oKernels),   spec);
    if (p.isSet(oNorms))     applyField("norms",     p.value(oNorms),     spec);
    if (p.isSet(oScalings))  applyField("scalings",  p.value(oScalings),  spec);
    if (p.isSet(oMechs))      applyField("mechs",          p.value(oMechs),      spec);
    if (p.isSet(oReduces))    applyField("reduces",        p.value(oReduces),    spec);
    if (p.isSet(oAffinities)) applyField("affinities",     p.value(oAffinities), spec);
    if (p.isSet(oGpuPrec))    applyField("gpu_precisions", p.value(oGpuPrec),    spec);
    if (p.isSet(oGpuWg))      applyField("gpu_workgroups", p.value(oGpuWg),      spec);
    if (p.isSet(oGpuPpw))     applyField("gpu_ppws",       p.value(oGpuPpw),     spec);
    if (p.isSet(oThreads))   applyField("threads",   p.value(oThreads),   spec);
    if (p.isSet(oSchedules)) applyField("schedules", p.value(oSchedules), spec);
    if (p.isSet(oChunks))    applyField("chunks",    p.value(oChunks),    spec);
    if (p.isSet(oImages) || p.isSet(oSynthetic)) {
        spec.images.clear();
        for (const auto& v : p.values(oImages))    for (const auto& s : splitList(v)) spec.images.push_back(s.toStdString());
        for (const auto& v : p.values(oSynthetic)) spec.images.push_back("synthetic:" + v.toStdString());
    }
    auto intOpt = [&](const QCommandLineOption& o, int& dst, int minV) {
        if (!p.isSet(o)) return;
        bool ok = false;
        const int v = p.value(o).toInt(&ok);
        if (!ok || v < minV) failArgs(QString("zla wartosc --%1: %2").arg(o.names().first(), p.value(o)));
        dst = v;
    };
    intOpt(oWarmup, spec.warmup, 0);
    intOpt(oRepeats, spec.repeats, 1);
    if (p.isSet(oShuffle)) { int s = 0; intOpt(oShuffle, s, 0); spec.shuffleSeed = static_cast<std::uint32_t>(s); }
    if (p.isSet(oSaveImages)) spec.saveImages = true;
    if (p.isSet(oStrict))     spec.strict     = true;

    if (p.isSet(oVerify))
        return runVerifyMode(spec, p.isSet(oVariants) || rawExperiment.contains("variants"),
                             p.isSet(oMasks) || rawExperiment.contains("masks"),
                             p.isSet(oImages) || p.isSet(oSynthetic) || rawExperiment.contains("images"),
                             p.isSet(oKernels) || rawExperiment.contains("kernels"),
                             p.isSet(oNorms) || rawExperiment.contains("norms"),
                             p.isSet(oScalings) || rawExperiment.contains("scalings"));

    // --- kontrole przed pomiarem ------------------------------------------------
    if (!p.isSet(oOut)) failArgs("brak --out KATALOG");
    if (spec.images.empty()) failArgs("brak obrazow (--image / --synthetic / pole images)");
    if (isDebugBuild() && !p.isSet(oAllowDebug))
        failArgs(QString("build '%1' – pomiary tylko w Release (albo --allow-debug)").arg(EDGE_BUILD_TYPE));

    for (const auto& n : spec.variants) {
        if (n == "all" || n == "ALL") continue;
        const VariantInfo* v = findVariant(n);
        if (!v) failArgs(QString("nieznany wariant: %1").arg(QString::fromStdString(n)));
        if (!v->available && spec.strict) {
            logErr(QString("edge_bench: wariant %1 niedostepny: %2").arg(v->name, v->unavailableReason));
            emitEvent("error", { { "code", ExitUnavailable }, { "message", QString("wariant %1 niedostepny").arg(v->name) } });
            return ExitUnavailable;
        }
    }
    for (const auto& m : spec.masks)
        if (!findMask(m)) failArgs(QString("nieznana maska: %1").arg(QString::fromStdString(m)));

    const QString outDir = QDir::cleanPath(p.value(oOut));
    QDir od(outDir);
    if (od.exists() && !od.isEmpty()) {
        if (!p.isSet(oForce)) failArgs("katalog wynikow istnieje i nie jest pusty: " + outDir + " (uzyj --force)");
        if (!od.removeRecursively()) failArgs("nie mozna wyczyscic " + outDir);
    }
    if (!QDir().mkpath(outDir)) {
        logErr("edge_bench: nie mozna utworzyc " + outDir);
        return ExitIo;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // --- meta.json (wstepny – nadpisywany na koncu) -----------------------------
    QJsonObject meta{
        { "experiment", QString::fromStdString(spec.name) },
        { "description", rawExperiment.value("description").toString() },
        { "started", QDateTime::currentDateTime().toString(Qt::ISODate) },
        { "git", gitDescribe() },
        { "command_line", QCoreApplication::arguments().join(' ') },
        { "working_dir", QDir::currentPath() },
        { "experiment_file", p.value(oExperiment) },
        { "spec", specJson(spec) },
        { "system", systemInfo() },
        { "complete", false },
    };
    auto writeMeta = [&] {
        QFile f(QDir(outDir).filePath("meta.json"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
        return true;
    };
    if (!writeMeta()) { logErr("edge_bench: nie mozna zapisac meta.json"); return ExitIo; }

    if (governor() != "performance" && governor() != "unknown")
        emitEvent("warning", { { "message", "governor CPU = " + governor() + " (zalecany performance)" } });

    QJsonArray warnings;
    HarnessCallbacks cb;
    cb.stopRequested = [] { return g_stop.load(); };
    cb.warning = [&](const std::string& m) {
        warnings.append(QString::fromStdString(m));
        emitEvent("warning", { { "message", QString::fromStdString(m) } });
    };
    cb.progress = [](const HarnessProgress& pr) {
        emitEvent("progress", { { "done", static_cast<qint64>(pr.done) }, { "total", static_cast<qint64>(pr.total) },
                                { "variant", QString::fromStdString(pr.variant) },
                                { "image", QString::fromStdString(pr.image) },
                                { "mask", QString::fromStdString(pr.mask) }, { "threads", pr.threads },
                                { "median_ms", pr.lastMedianMs } });
    };

    HarnessSummary sum;
    try {
        sum = runExperiment(spec, outDir.toStdString(), cb);
    } catch (const std::exception& e) {
        logErr(QString("edge_bench: ") + e.what());
        meta.insert("error", e.what());
        writeMeta();
        emitEvent("error", { { "code", ExitArgs }, { "message", e.what() } });
        return ExitArgs;
    }

    // --- meta.json koncowy ------------------------------------------------------
    QJsonArray imgs;
    for (const auto& im : sum.images) {
        const QString src = QString::fromStdString(im.source);
        QJsonObject o{ { "source", src }, { "width", im.width }, { "height", im.height } };
        if (!src.startsWith("synthetic:")) o.insert("sha256", sha256OfFile(src));
        imgs.append(o);
    }
    QJsonArray ran, skipped;
    for (const auto& v : sum.runVariants)     ran.append(QString::fromStdString(v));
    for (const auto& v : sum.skippedVariants) skipped.append(QString::fromStdString(v));
    meta.insert("finished", QDateTime::currentDateTime().toString(Qt::ISODate));
    meta.insert("complete", sum.complete);
    meta.insert("interrupted", g_stop.load());
    meta.insert("cases_planned", static_cast<qint64>(sum.casesPlanned));
    meta.insert("cases_done", static_cast<qint64>(sum.casesDone));
    meta.insert("rows", static_cast<qint64>(sum.rowsWritten));
    meta.insert("correctness_failures", static_cast<qint64>(sum.correctnessFailures));
    meta.insert("variants_run", ran);
    meta.insert("variants_skipped", skipped);
    meta.insert("images", imgs);
    meta.insert("warnings", warnings);
    meta.insert("peak_ram_mb", peakRamBytes() / (1024.0 * 1024.0));
    if (sum.ioError) meta.insert("error", QString::fromStdString(sum.errorMessage));
    if (!writeMeta()) { logErr("edge_bench: nie mozna zapisac meta.json"); return ExitIo; }

    int code = ExitOk;
    if (sum.ioError)                     code = ExitIo;
    else if (g_stop.load())              code = ExitInterrupted;
    else if (sum.correctnessFailures)    code = ExitCorrectness;

    emitEvent("done", { { "out", outDir }, { "complete", sum.complete }, { "rows", static_cast<qint64>(sum.rowsWritten) },
                        { "correctness_failures", static_cast<qint64>(sum.correctnessFailures) }, { "exit_code", code } });
    if (sum.ioError) logErr("edge_bench: " + QString::fromStdString(sum.errorMessage));
    return code;
}
