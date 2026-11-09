#include "masks.h"

#include <cstdlib>

namespace {

Mask makeSeparable(const std::string& id, const std::string& name,
                   const std::vector<int>& smooth, const std::vector<int>& diff)
{
    Mask m;
    m.id = id;
    m.name = name;
    m.size = static_cast<int>(smooth.size());
    m.separable = true;
    m.smooth = smooth;
    m.diff = diff;
    const int k = m.size;
    m.gx.resize(static_cast<size_t>(k) * k);
    m.gy.resize(static_cast<size_t>(k) * k);
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < k; ++j) {
            m.gx[static_cast<size_t>(i) * k + j] = smooth[i] * diff[j];
            m.gy[static_cast<size_t>(i) * k + j] = diff[i] * smooth[j];
        }
    return m;
}

std::vector<Mask> buildMasks()
{
    std::vector<Mask> v;
    v.push_back(makeSeparable("sobel3", "Sobel 3x3", { 1, 2, 1 }, { -1, 0, 1 }));
    v.push_back(makeSeparable("sobel5", "Sobel 5x5", { 1, 4, 6, 4, 1 }, { -1, -2, 0, 2, 1 }));
    v.push_back(makeSeparable("prewitt", "Prewitt 3x3", { 1, 1, 1 }, { -1, 0, 1 }));
    v.push_back(makeSeparable("scharr", "Scharr 3x3", { 3, 10, 3 }, { -1, 0, 1 }));

    // Roberts to maska 2x2 – wpisana w 3x3 z zerami; nie jest rozdzielna
    Mask r;
    r.id = "roberts";
    r.name = "Roberts (krzyz)";
    r.size = 3;
    r.gx = { 0, 0, 0,
             0, 1, 0,
             0, 0,-1 };
    r.gy = { 0, 0, 0,
             0, 0, 1,
             0,-1, 0 };
    v.push_back(r);

    v.push_back(makeSeparable("sobel7", "Sobel 7x7", { 1, 6, 15, 20, 15, 6, 1 },
                              { -1, -4, -5, 0, 5, 4, 1 }));
    return v;
}

} // namespace

const std::vector<Mask>& availableMasks()
{
    static const std::vector<Mask> masks = buildMasks();
    return masks;
}

const Mask* findMask(const std::string& idOrNumber)
{
    const auto& masks = availableMasks();
    for (const auto& m : masks)
        if (m.id == idOrNumber) return &m;

    char* end = nullptr;
    const long n = std::strtol(idOrNumber.c_str(), &end, 10);
    if (end && *end == '\0' && n >= 1 && n <= static_cast<long>(masks.size()))
        return &masks[static_cast<size_t>(n - 1)];
    return nullptr;
}
