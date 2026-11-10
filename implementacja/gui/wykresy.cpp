/**
 * @file wykresy.cpp
 * @brief Implementacja wspólnego wyglądu wykresów (patrz wykresy.h).
 */
#include "wykresy.h"

#include <QApplication>
#include <QChart>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QLegend>
#include <QMenu>
#include <QValueAxis>

QChart* nowyWykres(const QString& tytul)
{
    auto* c = new QChart;
    c->setAnimationOptions(QChart::NoAnimation);
    c->setTitle(tytul);
    QFont f = QApplication::font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 1.05);
    c->setTitleFont(f);
    c->legend()->setAlignment(Qt::AlignBottom);
    c->legend()->setFont(QApplication::font());
    c->setBackgroundRoundness(6);
    c->setMargins(QMargins(6, 6, 6, 6));
    c->setLocalizeNumbers(true);
    c->setLocale(QLocale(QLocale::Polish));
    return c;
}

QValueAxis* osWartosci(const QString& podpis, const QString& format)
{
    auto* a = new QValueAxis;
    a->setTitleText(podpis);
    a->setLabelFormat(format);
    a->setGridLineColor(QColor(0, 0, 0, 30));
    a->setMinorGridLineVisible(false);
    QFont f = QApplication::font();
    a->setTitleFont(f);
    a->setLabelsFont(f);
    return a;
}

WidokWykresu::WidokWykresu(QChart* wykres, QWidget* parent) : QChartView(wykres, parent)
{
    setRenderHint(QPainter::Antialiasing);
    setMinimumHeight(fontMetrics().height() * 13);
    setToolTip(QStringLiteral("Prawy przycisk myszy: zapisz wykres jako PNG."));
}

void WidokWykresu::zapiszPng()
{
    const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Zapisz wykres"), QStringLiteral("wykres.png"),
                                                   QStringLiteral("Obraz PNG (*.png)"));
    if (!f.isEmpty()) grab().save(f);
}

void WidokWykresu::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu m(this);
    m.addAction(QStringLiteral("Zapisz wykres jako PNG…"), this, &WidokWykresu::zapiszPng);
    m.exec(e->globalPos());
}
