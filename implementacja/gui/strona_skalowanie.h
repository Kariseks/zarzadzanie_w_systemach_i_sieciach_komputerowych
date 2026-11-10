/**
 * @file strona_skalowanie.h
 * @brief Zakładka „Skalowanie wątków”: przegląd liczby wątków W4, S(N), E(N) i prawo Amdahla.
 */
#pragma once

#include <QMap>
#include <QWidget>

#include "engine.h"
#include "presets.h"

class QChart;
class QLabel;
class QLineSeries;
class QProgressBar;
class QPushButton;
class QScatterSeries;
class QSpinBox;
class QTableWidget;
class QValueAxis;
class ParametryWersji;
class WidokWykresu;
class WyborMaski;
class WyborObrazu;
class Wykonawca;

/**
 * @brief Mierzy W4 dla N = 1…Nmax wątków i rysuje przyspieszenie S(N) = T(1)/T(N)
 *        z linią idealną S = N i krzywą prawa Amdahla dopasowaną do pomiarów, a pod nim efektywność E(N).
 *
 * Punkty pojawiają się na wykresie po każdym zmierzonym N.
 */
class StronaSkalowanie : public QWidget {
    Q_OBJECT
public:
    explicit StronaSkalowanie(QWidget* parent = nullptr);
    /** @brief Stosuje gotowe ustawienie (nie uruchamia pomiaru). */
    void zastosuj(const GotoweUstawienie& u);
    /** @brief Uruchamia przegląd liczby wątków. */
    void uruchom();

    /**
     * @brief Dopasowuje udział części sekwencyjnej s w prawie Amdahla S(N) = 1 / (s + (1 − s)/N)
     *        metodą najmniejszych kwadratów (przegląd s co 0,001).
     */
    static double dopasujAmdahla(const QVector<QPointF>& sn);

signals:
    /** @brief Komunikat do paska stanu. */
    void komunikat(const QString& tekst, int ms = 6000);

private:
    void odswiez();
    void odswiezBlokade();
    QVector<int> listaWatkow() const;

    WyborObrazu* obraz_ = nullptr;
    WyborMaski* maska_ = nullptr;
    ParametryWersji* param_ = nullptr;
    QSpinBox* maksWatkow_ = nullptr;
    QSpinBox* powtorzenia_ = nullptr;
    QPushButton* uruchom_ = nullptr;
    QPushButton* przerwij_ = nullptr;
    QProgressBar* postep_ = nullptr;
    QLabel* podsumowanie_ = nullptr;

    QChart* wykS_ = nullptr;
    QChart* wykE_ = nullptr;
    QLineSeries* sZmierzone_ = nullptr;
    QScatterSeries* sPunkty_ = nullptr;
    QLineSeries* sIdealne_ = nullptr;
    QLineSeries* sAmdahl_ = nullptr;
    QLineSeries* sE_ = nullptr;
    QScatterSeries* sEPunkty_ = nullptr;
    QValueAxis *osSX_ = nullptr, *osSY_ = nullptr, *osEX_ = nullptr, *osEY_ = nullptr;
    QTableWidget* tabela_ = nullptr;

    Wykonawca* wyk_ = nullptr;
    QVector<int> watki_;           ///< liczby wątków w bieżącym przeglądzie
    QMap<int, Pomiar> wyniki_;     ///< N → pomiar
};
