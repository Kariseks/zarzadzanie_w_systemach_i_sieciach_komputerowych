/**
 * @file historia.h
 * @brief Zakładka „Historia pomiarów”: wszystkie pomiary bieżącej sesji z parametrami i warunkami.
 */
#pragma once

#include <QVector>
#include <QWidget>

#include "engine.h"

class QLabel;
class QTableWidget;

/**
 * @brief Tabela wszystkich pomiarów sesji (ze wszystkich zakładek) z parametrami, statystyką i warunkami.
 *
 * Służy do szybkich, rzetelnych testów: każdy pomiar jest zapisany razem z warunkami
 * (zasilanie, profil, regulator, zajętość CPU) i współczynnikiem zmienności, a całość
 * można wyeksportować do CSV. Pomiary niestabilne (CV > 5 %) są oznaczone.
 */
class StronaHistoria : public QWidget {
    Q_OBJECT
public:
    explicit StronaHistoria(QWidget* parent = nullptr);
    /** @brief Dopisuje pomiar na górze tabeli. */
    void dodaj(const Pomiar& p);
    /** @brief Zapisuje całą historię do CSV (okno wyboru pliku). */
    void eksportujCsv();
    /** @brief Czyści historię. */
    void wyczysc();
    /** @brief Liczba zapisanych pomiarów. */
    int liczbaPomiarow() const { return pomiary_.size(); }

signals:
    /** @brief Komunikat do paska stanu. */
    void komunikat(const QString& tekst, int ms = 6000);

private:
    void odswiezPodsumowanie();
    QTableWidget* tabela_ = nullptr;
    QLabel* podsumowanie_ = nullptr;
    QVector<Pomiar> pomiary_;
};

/** @brief Zwięzły opis parametrów pomiaru, np. „24 w., dynamic, 16 w.” dla W4 albo „16×16, 1 px” dla W5. */
QString opisParametrow(const Zlecenie& z);
