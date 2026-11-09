#include "verify.h"

#include <stdexcept>
#include <tuple>

#include "synthetic.h"

std::vector<std::string> defaultVerifyImages()
{
    return {
        "synthetic:1x1:1",        // pojedynczy piksel – wszystkie sasiady to brzeg
        "synthetic:1x37:2",       // jedna kolumna
        "synthetic:41x1:3",       // jeden wiersz (mniej wierszy niz watkow)
        "synthetic:2x3:4",        // mniejszy niz maska 5x5 i 7x7
        "synthetic:7x5:5",        // wymiary nieparzyste
        "synthetic:33x17:6",      // wierszy mniej niz 32 watki
        "synthetic:640x480:7",
        "synthetic:1921x1081:8",  // nieparzyste, wiecej niz 1 fragment na watek
    };
}

namespace {

std::vector<const VariantInfo*> selectVariants(const std::vector<std::string>& names)
{
    std::vector<const VariantInfo*> out;
    if (names.empty()) {
        for (const auto& v : allVariants())
            if (v.available && v.id != VariantId::V0_Naive) out.push_back(&v);
        return out;
    }
    for (const auto& n : names) {
        if (n == "all") return selectVariants({});
        const VariantInfo* v = findVariant(n);
        if (!v) throw std::invalid_argument("nieznany wariant: " + n);
        if (v->available) out.push_back(v);
    }
    return out;
}

std::vector<const VariantInfo*> selectKernels(const std::vector<std::string>& names)
{
    std::vector<const VariantInfo*> out;
    if (names.empty()) {
        for (const auto& v : allVariants())
            if (isKernelVariant(v)) out.push_back(&v);
        return out;
    }
    for (const auto& n : names) {
        const VariantInfo* v = findVariant(n);
        if (!v || !isKernelVariant(*v)) throw std::invalid_argument("niepoprawne jadro: " + n);
        out.push_back(v);
    }
    return out;
}

std::vector<const Mask*> selectMasks(const std::vector<std::string>& ids)
{
    std::vector<const Mask*> out;
    if (ids.empty()) {
        for (const auto& m : availableMasks()) out.push_back(&m);
        return out;
    }
    for (const auto& id : ids) {
        const Mask* m = findMask(id);
        if (!m) throw std::invalid_argument("nieznana maska: " + id);
        out.push_back(m);
    }
    return out;
}

bool withinTolerance(const VariantInfo& v, const Comparison& c)
{
    if (!c.sameSize) return false;
    if (v.maxAllowedDiff == kApproximate) return true;
    return c.maxAbsDiff <= v.maxAllowedDiff;
}

struct Planned {
    const VariantInfo* variant;
    const VariantInfo* kernel;   // tylko T
    RunConfig          cfg;      // bez norm/scaling (ustawiane w petli)
};

} // namespace

std::vector<VerifyCase> runVerification(const VerifyOptions& opt,
                                        const std::function<void(size_t, size_t)>& progress)
{
    const auto variants = selectVariants(opt.variants);
    const auto kernels  = selectKernels(opt.kernels);
    const auto masks    = selectMasks(opt.masks);
    const auto images   = opt.images.empty() ? defaultVerifyImages() : opt.images;

    std::vector<Planned> plan;
    for (const VariantInfo* v : variants) {
        if (!v->multithreaded) {
            plan.push_back({ v, nullptr, RunConfig{} });
            if (v->id == VariantId::GPU_OpenCL || v->id == VariantId::W5)
                for (auto [wx, wy, ppw] : { std::tuple{ 8, 8, 3 }, std::tuple{ 32, 8, 16 }, std::tuple{ 32, 32, 2 } }) {
                    RunConfig c;
                    c.gpuWgX = wx;  c.gpuWgY = wy;  c.gpuPpw = ppw;
                    plan.push_back({ v, nullptr, c });
                }
            continue;
        }
        const std::vector<const VariantInfo*> ks =
            v->id == VariantId::W4 ? std::vector<const VariantInfo*>{ &variantInfo(VariantId::V0_Naive) } : kernels;
        for (const VariantInfo* k : ks) {
            for (RunConfig c : opt.mtConfigs) {
                c.kernel = k->id;
                plan.push_back({ v, k, c });
            }
            if (!isThreadPoolVariant(v->id)) continue;
            // Mechanizmy T: kazdy sposob redukcji, nowe watki, przypinanie do rdzeni
            for (Reduce red : { Reduce::Atomic, Reduce::Mutex, Reduce::PixelPadded, Reduce::PixelUnpadded }) {
                RunConfig c{ 7, Schedule::Dynamic, 3 };
                c.kernel = k->id;  c.reduce = red;
                plan.push_back({ v, k, c });
            }
            for (Affinity aff : { Affinity::Compact, Affinity::Spread, Affinity::NoSmt }) {
                RunConfig c{ 32, Schedule::Guided, 2 };
                c.kernel = k->id;  c.affinity = aff;  c.mech = ThreadMech::Spawn;
                plan.push_back({ v, k, c });
            }
        }
    }

    const size_t total = plan.size() * images.size() * masks.size() *
                         opt.norms.size() * opt.scalings.size();
    std::vector<VerifyCase> cases;
    cases.reserve(total);
    VariantRunner runner;
    size_t done = 0;

    for (const auto& src : images) {
        GrayImage img;
        if (!loadOrGenerate(src, img))
            throw std::runtime_error("nie mozna wczytac obrazu: " + src);

        for (const Mask* m : masks)
            for (Norm norm : opt.norms)
                for (Scaling sc : opt.scalings) {
                    RunConfig refCfg;
                    refCfg.norm = norm;
                    refCfg.scaling = sc;
                    const RunResult ref = runner.run(VariantId::V0_Naive, img, *m, refCfg);

                    for (const Planned& p : plan) {
                        VerifyCase vc;
                        vc.variant       = p.variant->name;
                        vc.kernel        = p.kernel ? p.kernel->name : "";
                        vc.image         = src;
                        vc.width         = img.width;
                        vc.height        = img.height;
                        vc.mask          = m->id;
                        vc.cfg           = p.cfg;
                        vc.cfg.norm      = norm;
                        vc.cfg.scaling   = sc;
                        vc.multithreaded = p.variant->multithreaded;
                        try {
                            RunResult r = runner.run(p.variant->id, img, *m, vc.cfg);
                            vc.cmp  = compareImages(ref.output, r.output);
                            vc.pass = withinTolerance(*p.variant, vc.cmp);
                            runner.recycle(r);   // kolejne wywolanie dostaje stary bufor – sprawdza, ze wariant nadpisuje kazdy piksel
                        } catch (const std::exception& e) {
                            vc.error = e.what();
                            vc.pass  = false;
                        }
                        cases.push_back(std::move(vc));
                        if (progress) progress(++done, total);
                    }
                }
    }
    return cases;
}
