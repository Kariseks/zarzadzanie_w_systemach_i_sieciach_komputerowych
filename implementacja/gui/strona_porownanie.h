/**
 * @file strona_porownanie.h
 * @brief Zakładka „Porównanie wersji”: W1–W5 na tym samym obrazie, wykresy i tabela.
 */
#pragma once

#include <QMap>
#include <QWidget>

#include "engine.h"
#include "presets.h"

class QCheckBox;
class QChart;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class ParametryWersji;
class WidokWykresu;
class WyborMaski;
class WyborObrazu;
class Wykonawca;

/**
 * @brief Zakładka porównania wersji.
 *
 * Górny wykres: czas samego algorytmu każdej wersji z przyspieszeniem względem W1.
 * Dolny wykres: cały program z podziałem na wczytanie, algorytm i zapis – pokazuje,
 * dlaczego przyspieszenie całego programu jest dużo mniejsze (prawo Amdahla).
 */
class StronaPorownanie : public QWidget {
    Q_OBJECT
public:
    explicit StronaPorownanie(QWidget* parent = nullptr);
    /** @brief Stosuje gotowe ustawienie (nie uruchamia pomiaru). */
    void zastosuj(const GotoweUstawienie& u);
    /** @brief Uruchamia pomiar zaznaczonych wersji po kolei. */
    void uruchom();
    /** @brief Zapisuje tabelę wyników do CSV (okno wyboru pliku). */
    void eksportujCsv();

signals:
    /** @brief Komunikat do paska stanu. */
    void komunikat(const QString& tekst, int ms = 6000);

private:
    void odswiez();
    void odswiezBlokade();

    WyborObrazu* obraz_ = nullptr;
    WyborMaski* maska_ = nullptr;
    QMap<Wersja, QCheckBox*> zaznacz_;
    ParametryWersji* paramW4_ = nullptr;
    ParametryWersji* paramW5_ = nullptr;
    QSpinBox* powtorzenia_ = nullptr;
    QCheckBox* zPlikiem_ = nullptr;
    QPushButton* uruchom_ = nullptr;
    QPushButton* przerwij_ = nullptr;
    QProgressBar* postep_ = nullptr;
    QLabel* podsumowanie_ = nullptr;

    QChart* wykAlg_ = nullptr;
    QChart* wykProg_ = nullptr;
    WidokWykresu* widokAlg_ = nullptr;
    WidokWykresu* widokProg_ = nullptr;
    QTableWidget* tabela_ = nullptr;

    Wykonawca* wyk_ = nullptr;
    QMap<Wersja, Pomiar> wyniki_;
};
