/**
 * @file wykresy.h
 * @brief Wspólny wygląd wykresów Qt Charts w edge_gui (zgodny z rysunkami w sprawozdaniu).
 */
#pragma once

#include <QChartView>
#include <QString>

class QAbstractAxis;
class QChart;
class QValueAxis;

/**
 * @brief Tworzy wykres w stylu aplikacji: bez animacji (nie zabierają CPU w trakcie pomiaru),
 *        legenda pod wykresem, cienka siatka, czcionka interfejsu.
 * @param tytul tytuł wykresu (krótki, nad wykresem)
 */
QChart* nowyWykres(const QString& tytul);

/** @brief Oś wartości z podpisem i polskim formatem liczb. */
QValueAxis* osWartosci(const QString& podpis, const QString& format = QStringLiteral("%.1f"));

/** @brief Widok wykresu z wygładzaniem i menu kontekstowym „Zapisz jako PNG…”. */
class WidokWykresu : public QChartView {
    Q_OBJECT
public:
    explicit WidokWykresu(QChart* wykres, QWidget* parent = nullptr);
    /** @brief Zapis wykresu do pliku PNG (okno wyboru pliku). */
    void zapiszPng();

protected:
    void contextMenuEvent(QContextMenuEvent* e) override;
};
