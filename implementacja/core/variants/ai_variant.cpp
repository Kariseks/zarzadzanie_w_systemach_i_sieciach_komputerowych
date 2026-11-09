// ============================================================================
//  AI – wersja zaproponowana przez model AI (procedura: implementacja/ai/PROMPT.md)
//
//  Narzedzie / model / data: (uzupelnic)
//  Dopasowanie do sygnatury rowsAI: (opisac, co zmieniono w kodzie z odpowiedzi)
//
//  Dopoki aiVariantImplemented() zwraca false, wariant AI jest oznaczony
//  jako niedostepny i pomijany w pomiarach.
// ============================================================================
#include <stdexcept>

#include "../kernels.h"

bool aiVariantImplemented() { return false; }

namespace {

// Tu wkleic funkcje z odpowiedzi AI (bez poprawek merytorycznych), np.:
// float edgeRows(const GrayImage& in, float* mag, const Mask& m, Norm norm, int y0, int y1) { ... }

} // namespace

float rowsAI(const KernelArgs& a, int y0, int y1, Scratch&)
{
    (void)a; (void)y0; (void)y1;
    // Przyklad dopasowania (prompt dotyczy trybu z buforem modulu):
    //   if (a.mag) return edgeRows(a.img, a.mag, a.mask, a.norm, y0, y1);
    //   tryby Fixed/Clamp: policzyc do bufora tymczasowego i zamienic na bajty toByte()
    throw std::runtime_error("wariant AI nie jest jeszcze wklejony (implementacja/ai/PROMPT.md)");
}
