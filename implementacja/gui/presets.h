/**
 * @file presets.h
 * @brief Gotowe ustawienia do pokazu: każde ustawia zakładkę, obraz, maskę, wersję i parametry,
 *        tak żeby jednym wyborem pokazać konkretne zjawisko z wyników projektu.
 */
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "engine.h"

/** @brief Zakładki głównego okna (kolejność jak w QTabWidget). */
enum class Zakladka { Przetwarzanie = 0, Porownanie = 1, Skalowanie = 2, Historia = 3, System = 4 };

/** @brief Jedno gotowe ustawienie. */
struct GotoweUstawienie {
    QString   nazwa;        ///< tekst w menu i na liście
    QString   opis;         ///< co pokazuje i czego się spodziewać (podpowiedź)
    Zakladka  zakladka = Zakladka::Przetwarzanie;
    QString   obraz;        ///< ścieżka względem katalogu repozytorium albo „synthetic:…”
    QString   maska = "sobel3";
    Wersja    wersja = Wersja::W1;
    RunConfig cfg;          ///< parametry W4 / W5
    int       powtorzenia = 5;
    bool      zWczytaniem = true;
    int       maksWatkow = 24; ///< tylko zakładka Skalowanie
};

/** @brief Lista gotowych ustawień (kolejność = kolejność pokazu). */
const QVector<GotoweUstawienie>& gotoweUstawienia();

/** @brief Zamienia ścieżkę z ustawienia na pełną (względem katalogu repozytorium). */
QString pelnaSciezka(const QString& obraz);

/**
 * @brief Koordynator pomiarów: w danej chwili może trwać tylko jeden pomiar
 *        (dwa jednocześnie zabierałyby sobie rdzenie i fałszowały czasy).
 */
class Koordynator : public QObject {
    Q_OBJECT
public:
    /** @brief Jedyny egzemplarz. */
    static Koordynator& instancja();
    /** @brief Czy trwa pomiar. */
    bool zajety() const { return zajety_; }
    /** @brief Zgłasza początek (true) albo koniec (false) pomiaru. */
    void ustawZajety(bool z, const QString& opis = {});
    /** @brief Przekazuje zakończony pomiar wszystkim zainteresowanym (historia pomiarów). */
    void zglosPomiar(const Pomiar& p) { emit nowyPomiar(p); }

signals:
    /** @brief Nowy zakończony pomiar (z dowolnej zakładki). */
    void nowyPomiar(const Pomiar& p);
    /** @brief Zmiana stanu – strony blokują przyciski „Uruchom”. */
    void zmiana(bool zajety, const QString& opis);

private:
    bool zajety_ = false;
};
