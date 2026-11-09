#include "harness.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "compare.h"
#include "measure.h"
#include "synthetic.h"
#include "variants.h"

namespace fs = std::filesystem;

namespace {

int hwThreads()
{
    const int n = static_cast<int>(std::thread::hardware_concurrency());
    return n > 0 ? n : 1;
}

// --- CSV ---------------------------------------------------------------------
std::string csvField(const std::string& s)
{
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    return out + "\"";
}

std::string num(double v, int precision = 4)
{
    std::ostringstream os;
    os.imbue(std::locale::classic());              // zawsze kropka dziesietna
    os << std::fixed << std::setprecision(precision) << v;
    return os.str();
}

std::string psnrField(double v) { return formatPsnr(v); }

// Prosty xorshift do tasowania (deterministyczny niezaleznie od STL)
void shuffleDeterministic(std::vector<size_t>& idx, std::uint32_t seed)
{
    std::uint32_t s = seed ? seed : 1u;
    auto next = [&] { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    for (size_t i = idx.size(); i > 1; --i)
        std::swap(idx[i - 1], idx[next() % i]);
}

std::string safeName(std::string s)
{
    for (char& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) c = '_';
    return s;
}

double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

struct Case {
    const VariantInfo* variant;
    const VariantInfo* kernel;   // tylko T
    RunConfig          cfg;      // bez norm / scaling
};

// Kolumny identyfikujace przypadek (wspolne dla obu plikow)
#define EDGE_ID_COLS "experiment", "variant", "kernel", "image", "width", "height", "mask", \
                     "norm", "scaling", "threads", "schedule", "chunk", \
                     "mech", "reduce", "affinity", "gpu_prec", "gpu_wg", "gpu_ppw"

const std::vector<std::string> kResultCols = {
    EDGE_ID_COLS, "rep",
    "wall_ms", "cpu_ms", "conv_ms", "normalize_ms", "upload_ms", "kernel_ms", "download_ms",
    "imbalance_ms", "peak_ram_mb", "process_ms", "load_ms", "save_ms", "gpu_items",
};

// l2_* = porownanie z V0 z norma L2 (to samo skalowanie) – dokladnosc przyblizen L1 / Linf
const std::vector<std::string> kCorrectCols = {
    EDGE_ID_COLS,
    "identical", "max_abs_diff", "mean_abs_diff", "differing_pixels", "psnr_db", "tolerance", "pass",
    "l2_max_abs_diff", "l2_mean_abs_diff", "l2_psnr_db", "error",
};

void writeHeader(std::ofstream& f, const std::vector<std::string>& cols)
{
    for (size_t i = 0; i < cols.size(); ++i) f << (i ? "," : "") << cols[i];
    f << "\n";
}

std::string stage(const RunResult& r, const char* key)
{
    const auto it = r.stagesMs.find(key);
    return it == r.stagesMs.end() ? std::string() : num(it->second);
}

} // namespace

const std::vector<std::string>& resultsCsvColumns() { return kResultCols; }

bool parseIntList(const std::string& text, std::vector<int>& out, int maxValue)
{
    out.clear();
    if (maxValue <= 0) maxValue = hwThreads();
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item.erase(std::remove_if(item.begin(), item.end(), ::isspace), item.end());
        if (item.empty()) continue;
        if (item == "max") { out.push_back(maxValue); continue; }
        const size_t dash = item.find('-');
        char* end = nullptr;
        if (dash != std::string::npos && dash > 0) {
            const long a = std::strtol(item.substr(0, dash).c_str(), &end, 10);
            if (*end) return false;
            const std::string bs = item.substr(dash + 1);
            const long b = bs == "max" ? maxValue : std::strtol(bs.c_str(), &end, 10);
            if (bs != "max" && *end) return false;
            if (a < 0 || b < a || b - a > 10000) return false;
            for (long v = a; v <= b; ++v) out.push_back(static_cast<int>(v));
        } else {
            const long v = std::strtol(item.c_str(), &end, 10);
            if (*end || v < 0) return false;
            out.push_back(static_cast<int>(v));
        }
    }
    return !out.empty();
}

HarnessSummary runExperiment(const ExperimentSpec& spec, const std::string& outDir,
                             const HarnessCallbacks& cb)
{
    HarnessSummary sum;
    auto warn = [&](const std::string& m) { if (cb.warning) cb.warning(m); };
    auto stop = [&] { return cb.stopRequested && cb.stopRequested(); };

    // --- Warianty ------------------------------------------------------------
    std::vector<const VariantInfo*> variants;
    auto addVariant = [&](const VariantInfo* v) {
        if (std::find(variants.begin(), variants.end(), v) != variants.end()) return;
        if (!v->available) {
            sum.skippedVariants.push_back(v->name);
            warn(std::string("wariant ") + v->name + " pominiety: " + v->unavailableReason);
            return;
        }
        variants.push_back(v);
        sum.runVariants.push_back(v->name);
    };
    for (const auto& n : spec.variants) {
        if (n == "all" || n == "ALL") {
            for (const auto& v : allVariants()) addVariant(&v);
            continue;
        }
        const VariantInfo* v = findVariant(n);
        if (!v) throw std::invalid_argument("nieznany wariant: " + n);
        addVariant(v);
    }
    if (spec.strict && !sum.skippedVariants.empty())
        throw std::invalid_argument("niedostepne warianty przy --strict");
    if (variants.empty()) throw std::invalid_argument("brak wariantow do zmierzenia");

    // --- Jadra dla T ---------------------------------------------------------
    std::vector<const VariantInfo*> kernels;
    for (const auto& n : spec.kernels) {
        if (n == "all" || n == "ALL") {
            for (const auto& v : allVariants())
                if (isKernelVariant(v) && std::find(kernels.begin(), kernels.end(), &v) == kernels.end())
                    kernels.push_back(&v);
            continue;
        }
        const VariantInfo* v = findVariant(n);
        if (!v) throw std::invalid_argument("nieznane jadro: " + n);
        if (!isKernelVariant(*v)) {
            warn(std::string("jadro ") + v->name + " pominiete: " +
                 (v->available ? "nie jest wariantem 1-watkowym CPU" : v->unavailableReason));
            continue;
        }
        if (std::find(kernels.begin(), kernels.end(), v) == kernels.end()) kernels.push_back(v);
    }
    const bool hasMt = std::any_of(variants.begin(), variants.end(),
                                   [](const VariantInfo* v) { return v->multithreaded; });
    if (hasMt && kernels.empty()) throw std::invalid_argument("brak jadra dla wariantu wielowatkowego");

    // --- Maski ---------------------------------------------------------------
    std::vector<const Mask*> masks;
    for (const auto& id : spec.masks) {
        const Mask* m = findMask(id);
        if (!m) throw std::invalid_argument("nieznana maska: " + id);
        masks.push_back(m);
    }
    if (masks.empty()) throw std::invalid_argument("brak masek");
    if (spec.images.empty()) throw std::invalid_argument("brak obrazow");
    if (spec.repeats < 1) throw std::invalid_argument("repeats musi byc >= 1");
    if (spec.norms.empty() || spec.scalings.empty()) throw std::invalid_argument("brak norm / skalowan");

    // --- Przypadki (na obraz, maske, norme i skalowanie) ----------------------
    std::vector<Case> cases;
    for (const VariantInfo* v : variants) {
        const bool isGpu = isGpuVariant(v->id);
        if (!v->multithreaded) {
            if (isGpu) {
                const bool naiveGpu = v->id == VariantId::GPU_OpenCL || v->id == VariantId::W5;
                const std::vector<std::pair<int, int>> wgs =
                    naiveGpu ? spec.gpuWorkgroups : std::vector<std::pair<int, int>>{ { 16, 16 } };
                const std::vector<int> ppws = naiveGpu ? spec.gpuPpws : std::vector<int>{ 1 };
                for (bool f32 : spec.gpuFp32)
                    for (const auto& wg : wgs)
                        for (int ppw : ppws) {
                            RunConfig c{ 1, Schedule::Dynamic, 0 };
                            c.gpuFp32 = f32;
                            c.gpuWgX = wg.first;  c.gpuWgY = wg.second;  c.gpuPpw = ppw;
                            cases.push_back({ v, nullptr, c });
                        }
            } else {
                cases.push_back({ v, nullptr, RunConfig{ 1, Schedule::Dynamic, 0 } });
            }
            continue;
        }
        const bool isT = isThreadPoolVariant(v->id);
        const std::vector<ThreadMech> mechs = isT ? spec.mechs : std::vector<ThreadMech>{ ThreadMech::Pool };
        const std::vector<Reduce>     reds  = isT ? spec.reduces : std::vector<Reduce>{ Reduce::Local };
        const std::vector<Affinity>   affs  = isT ? spec.affinities : std::vector<Affinity>{ Affinity::None };
        const std::vector<const VariantInfo*> ks =
            v->id == VariantId::W4 ? std::vector<const VariantInfo*>{ &variantInfo(VariantId::V0_Naive) } : kernels;
        for (const VariantInfo* k : ks)
            for (int t : spec.threads)
                for (Schedule s : spec.schedules)
                    for (size_t ci = 0; ci < spec.chunks.size(); ++ci) {
                        if (s == Schedule::Static && ci > 0) continue;   // static nie uzywa chunka
                        for (ThreadMech me : mechs)
                            for (Reduce re : reds)
                                for (Affinity af : affs) {
                                    RunConfig c{ t > 0 ? t : hwThreads(), s, spec.chunks[ci] };
                                    c.kernel = k->id;
                                    c.mech = me;  c.reduce = re;  c.affinity = af;
                                    cases.push_back({ v, k, c });
                                }
                    }
    }
    sum.casesPlanned = cases.size() * masks.size() * spec.images.size() *
                       spec.norms.size() * spec.scalings.size();

    // --- Pliki ---------------------------------------------------------------
    fs::create_directories(outDir);
    if (spec.saveImages) fs::create_directories(fs::path(outDir) / "images");
    std::ofstream res(fs::path(outDir) / "results.csv");
    std::ofstream cor(fs::path(outDir) / "correctness.csv");
    if (!res || !cor) {
        sum.ioError = true;
        sum.errorMessage = "nie mozna utworzyc plikow CSV w " + outDir;
        return sum;
    }
    writeHeader(res, kResultCols);
    writeHeader(cor, kCorrectCols);

    VariantRunner runner;
    const std::string ioTmp = (fs::path(outDir) / "_io_tmp.jpg").string();

    for (const auto& src : spec.images) {
        if (stop()) break;
        GrayImage img;
        if (!loadOrGenerate(src, img)) {
            sum.ioError = true;
            sum.errorMessage = "nie mozna wczytac obrazu: " + src;
            return sum;
        }
        sum.images.push_back({ src, img.width, img.height });
        const std::string imgName = src;
        const std::string imgFile = safeName(src.rfind("synthetic:", 0) == 0 ? src : fs::path(src).stem().string());

        for (const Mask* mask : masks)
        for (Norm norm : spec.norms)
        for (Scaling scaling : spec.scalings) {
            if (stop()) break;
            RunConfig refCfg;
            refCfg.norm = norm;
            refCfg.scaling = scaling;
            const RunResult ref = runner.run(VariantId::V0_Naive, img, *mask, refCfg);
            RunResult refL2;
            if (norm != Norm::L2) {
                RunConfig c2 = refCfg;
                c2.norm = Norm::L2;
                refL2 = runner.run(VariantId::V0_Naive, img, *mask, c2);
            }
            const std::vector<std::uint8_t>& l2 = norm == Norm::L2 ? ref.output : refL2.output;

            std::vector<size_t> order(cases.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            if (spec.shuffleSeed) shuffleDeterministic(order, spec.shuffleSeed + static_cast<std::uint32_t>(sum.casesDone));

            for (size_t oi : order) {
                if (stop()) break;
                const Case& c = cases[oi];
                RunConfig cfg = c.cfg;
                cfg.norm = norm;
                cfg.scaling = scaling;
                const bool mt = c.variant->multithreaded;
                const std::string thr = std::to_string(mt ? cfg.threads : 1);
                const std::string sch = mt ? toString(cfg.schedule) : "";
                const std::string chk = (mt && cfg.schedule != Schedule::Static) ? std::to_string(cfg.chunkRows) : "";
                const std::string ker = c.kernel ? c.kernel->name : "";
                const bool isT   = isThreadPoolVariant(c.variant->id);
                const bool isGpu = isGpuVariant(c.variant->id);
                const std::string mechS = isT ? toString(cfg.mech) : "";
                const std::string redS  = (isT && scaling == Scaling::GlobalMax) ? toString(cfg.reduce) : "";
                const std::string affS  = isT ? toString(cfg.affinity) : "";
                const std::string gprS  = isGpu ? (cfg.gpuFp32 ? "fp32" : "auto") : "";
                const bool naiveGpu = c.variant->id == VariantId::GPU_OpenCL || c.variant->id == VariantId::W5;
                const std::string gwgS  = naiveGpu ? std::to_string(cfg.gpuWgX) + "x" + std::to_string(cfg.gpuWgY) : "";
                const std::string gppS  = naiveGpu ? std::to_string(cfg.gpuPpw) : "";
                const std::string prefix =
                    csvField(spec.name) + "," + csvField(c.variant->name) + "," + ker + "," + csvField(imgName) + "," +
                    std::to_string(img.width) + "," + std::to_string(img.height) + "," + mask->id + "," +
                    toString(norm) + "," + toString(scaling) + "," + thr + "," + sch + "," + chk + "," +
                    mechS + "," + redS + "," + affS + "," + gprS + "," + gwgS + "," + gppS;

                std::vector<double> times;
                try {
                    for (int w = 0; w < spec.warmup && !stop(); ++w) {
                        RunResult rw = runner.run(c.variant->id, img, *mask, cfg);
                        runner.recycle(rw);
                    }

                    for (int rep = 0; rep < spec.repeats; ++rep) {
                        if (stop()) break;
                        // E9: caly program = wczytanie pliku + przetwarzanie + zapis JPG
                        double loadMs = -1.0, saveMs = -1.0;
                        Stopwatch total;
                        GrayImage fresh;
                        const GrayImage* in = &img;
                        if (spec.includeIo) {
                            Stopwatch t;
                            if (!loadOrGenerate(src, fresh)) throw std::runtime_error("nie mozna wczytac " + src);
                            loadMs = t.wallMs();
                            in = &fresh;
                        }
                        RunResult r = runner.run(c.variant->id, *in, *mask, cfg);
                        const double processMs = r.wallMs;
                        if (spec.includeIo) {
                            Stopwatch t;
                            saveGrayJpg(ioTmp, r.output, img.width, img.height);
                            saveMs = t.wallMs();
                            r.wallMs = total.wallMs();
                        }
                        times.push_back(r.wallMs);
                        res << prefix << "," << rep << ","
                            << num(r.wallMs) << "," << num(r.cpuMs) << ","
                            << stage(r, "conv") << "," << stage(r, "norm") << ","
                            << stage(r, "upload") << "," << stage(r, "kernel") << "," << stage(r, "download") << ","
                            << (mt ? num(r.imbalanceMs) : std::string()) << ","
                            << num(peakRamBytes() / (1024.0 * 1024.0), 2) << ","
                            << num(processMs) << ","
                            << (loadMs >= 0 ? num(loadMs) : std::string()) << ","
                            << (saveMs >= 0 ? num(saveMs) : std::string()) << ","
                            << (r.gpuWorkItems ? std::to_string(r.gpuWorkItems) : std::string()) << "\n";
                        res.flush();
                        ++sum.rowsWritten;

                        if (rep == 0) {
                            const Comparison cmp = compareImages(ref.output, r.output);
                            const Comparison c2  = compareImages(l2, r.output);
                            const int tol = c.variant->maxAllowedDiff;
                            const bool pass = cmp.sameSize && (tol == kApproximate || cmp.maxAbsDiff <= tol);
                            if (!pass) ++sum.correctnessFailures;
                            cor << prefix << "," << (cmp.identical ? 1 : 0) << "," << cmp.maxAbsDiff << ","
                                << num(cmp.meanAbsDiff, 6) << "," << cmp.differing << "," << psnrField(cmp.psnrDb) << ","
                                << (tol == kApproximate ? std::string("approx") : std::to_string(tol)) << ","
                                << (pass ? 1 : 0) << ","
                                << c2.maxAbsDiff << "," << num(c2.meanAbsDiff, 6) << "," << psnrField(c2.psnrDb)
                                << ",\n";
                            cor.flush();
                            if (!pass)
                                warn(std::string("bledny wynik: ") + c.variant->name + (ker.empty() ? "" : "/" + ker) +
                                     " " + imgName + " " + mask->id + " " + toString(norm) + " " + toString(scaling) +
                                     " watki " + thr + " (maks. roznica " + std::to_string(cmp.maxAbsDiff) + ")");
                            if (spec.saveImages) {
                                std::string fn = std::string(c.variant->name) + (ker.empty() ? "" : "-" + ker) + "_" +
                                                 mask->id + "_" + toString(norm) + "_" + toString(scaling) + "_" + imgFile;
                                if (mt) fn += "_t" + thr + "_" + sch + (chk.empty() ? "" : "_c" + chk);
                                saveGrayPng((fs::path(outDir) / "images" / (fn + ".png")).string(),
                                            r.output, img.width, img.height);
                            }
                        }
                        runner.recycle(r);
                    }
                } catch (const std::exception& e) {
                    ++sum.correctnessFailures;
                    cor << prefix << ",0,,,,,,0,,,," << csvField(e.what()) << "\n";
                    cor.flush();
                    warn(std::string("wyjatek w wariancie ") + c.variant->name + ": " + e.what());
                }

                if (stop() && static_cast<int>(times.size()) < spec.repeats) break;  // przypadek niepelny
                ++sum.casesDone;
                if (cb.progress) {
                    HarnessProgress p;
                    p.done = sum.casesDone;  p.total = sum.casesPlanned;
                    p.variant = c.variant->name + (ker.empty() ? std::string() : "/" + ker);
                    p.image = imgName;  p.mask = mask->id;
                    p.threads = mt ? cfg.threads : 1;
                    p.lastMedianMs = median(times);
                    cb.progress(p);
                }
            }
        }
    }

    std::error_code ec;
    fs::remove(ioTmp, ec);
    sum.complete = sum.casesDone == sum.casesPlanned;
    return sum;
}
