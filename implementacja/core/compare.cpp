#include "compare.h"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

Comparison compareImages(const std::vector<std::uint8_t>& reference,
                         const std::vector<std::uint8_t>& tested)
{
    Comparison c;
    c.sameSize = reference.size() == tested.size();
    if (!c.sameSize || reference.empty()) {
        c.identical = c.sameSize;      // dwa puste obrazy sa identyczne
        c.psnrDb    = c.sameSize ? std::numeric_limits<double>::infinity() : 0.0;
        return c;
    }

    long long sumAbs = 0;
    double    sumSq  = 0.0;
    for (size_t i = 0; i < reference.size(); ++i) {
        const int d = std::abs(static_cast<int>(reference[i]) - static_cast<int>(tested[i]));
        if (d) {
            ++c.differing;
            sumAbs += d;
            sumSq  += static_cast<double>(d) * d;
            if (d > c.maxAbsDiff) c.maxAbsDiff = d;
        }
    }
    const double n = static_cast<double>(reference.size());
    c.identical   = c.differing == 0;
    c.meanAbsDiff = sumAbs / n;
    const double mse = sumSq / n;
    c.psnrDb = mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse)
                         : std::numeric_limits<double>::infinity();
    return c;
}

std::string formatPsnr(double psnrDb)
{
    if (std::isinf(psnrDb)) return "inf";
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << psnrDb;
    return os.str();
}
