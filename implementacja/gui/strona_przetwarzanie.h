/**
 * @file strona_przetwarzanie.h
 * @brief Zakładka „Przetwarzanie”: jedna wersja, obraz wejściowy i wynik obok siebie, czasy etapów.
 */
#pragma once

#include <QWidget>

#include "engine.h"
#include "presets.h"

class QButtonGroup;
class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class PasekCzasow;
class ParametryWersji;
class PodgladObrazu;
class WyborMaski;
class WyborObrazu;
class Wykonawca;

/**
 * @brief Zakładka do pokazu działania jednej wersji programu.
 *
 * Lewa kolumna – ustawienia (obraz, maska, wersja, parametry, liczba powtórzeń),
 * prawa część – obraz wejściowy i wynik (zsynchronizowane powiększenie) oraz
 * dwa paski czasu: cały program (wczytanie / algorytm / zapis) i etapy samego algorytmu.
 */
class StronaPrzetwarzanie : public QWidget {
    Q_OBJECT
public:
    explicit StronaPrzetwarzanie(QWidget* parent = nullptr);
    /** @brief Stosuje gotowe ustawienie (nie uruchamia pomiaru). */
    void zastosuj(const GotoweUstawienie& u);
    /** @brief Uruchamia pomiar z bieżącymi ustawieniami. */
    void uruchom();
    /** @brief Dopisuje plik do listy obrazów i go wybiera. */
    void otworzObraz(const QString& sciezka);
    /** @brief Zapisuje ostatni wynik do pliku (okno wyboru pliku). */
    void zapiszWynik();
    /** @brief Czy jest wynik do zapisania. */
    bool maWynik() const;

signals:
    /** @brief Komunikat do paska stanu. */
    void komunikat(const QString& tekst, int ms = 6000);

private:
    Wersja wersja() const;
    void pokaz(const Pomiar& p);
    void odswiezBlokade();

    WyborObrazu* obraz_ = nullptr;
    WyborMaski* maska_ = nullptr;
    QButtonGroup* wersje_ = nullptr;
    ParametryWersji* param_ = nullptr;
    QSpinBox* powtorzenia_ = nullptr;
    QCheckBox* zPlikiem_ = nullptr;
    QPushButton* uruchom_ = nullptr;
    QPushButton* przerwij_ = nullptr;
    QProgressBar* postep_ = nullptr;

    PodgladObrazu* wejscie_ = nullptr;
    PodgladObrazu* wyjscie_ = nullptr;
    PasekCzasow* pasekProgramu_ = nullptr;
    PasekCzasow* pasekAlgorytmu_ = nullptr;
    QLabel* duzyCzas_ = nullptr;
    QLabel* przyspieszenie_ = nullptr;
    QLabel* zgodnosc_ = nullptr;
    QLabel* szczegoly_ = nullptr;

    Wykonawca* wyk_ = nullptr;
    QImage ostatniWynik_;
};
