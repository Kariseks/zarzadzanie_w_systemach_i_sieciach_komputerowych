/**
 * @file strona_skalowanie.cpp
 * @brief Implementacja zakładki „Skalowanie wątków” (patrz strona_skalowanie.h).
 */
#include "strona_skalowanie.h"

#include <QApplication>
#include <QChart>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLegend>
#include <QLegendMarker>
#include <QLineSeries>
#include <QPen>
#include <QProgressBar>
#include <QPushButton>
#include <QScatterSeries>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QSet>
#include <QTableWidget>
#include <algorithm>
#include <QToolTip>
#include <QCursor>
#include <cmath>
#include <QValueAxis>
#include <QVBoxLayout>

#include "topology.h"
#include "widgets.h"
#include "wykonawca.h"
#include "wykresy.h"

namespace {
/** Przyspieszenie z prawa Amdahla. */
double amdahl(double n, double s) { return 1.0 / (s + (1.0 - s) / n); }
const QColor kKolor("#D55E00");   // kolor W4
} // namespace

double StronaSkalowanie::dopasujAmdahla(const QVector<QPointF>& sn)
{
    double najS = 0, najBlad = 1e300;
    for (int i = 0; i <= 1000; ++i) {
        const double s = i / 1000.0;
        double b = 0;
        for (const QPointF& p : sn) { const double d = p.y() - amdahl(p.x(), s); b += d * d; }
        if (b < najBlad) { najBlad = b; najS = s; }
    }
    return najS;
}

StronaSkalowanie::StronaSkalowanie(QWidget* parent) : QWidget(parent)
{
    setWhatsThis(QStringLiteral("<b>Skalowanie wątków</b><br>Mierzy W4 dla rosnącej liczby wątków. Przyspieszenie "
                                "S(N) = T(1)/T(N) porównywane jest z idealnym S = N i z prawem Amdahla "
                                "S(N) = 1 / (s + (1 − s)/N), gdzie s – udział części sekwencyjnej."));
    wyk_ = new Wykonawca(this);

    auto* ustawienia = new QWidget;
    auto* lu = new QVBoxLayout(ustawienia);
    auto* gDane = new QGroupBox(QStringLiteral("Dane"));
    gDane->setToolTip(QStringLiteral("Obraz wejściowy i maska (operator gradientowy). Te same dane dla każdej mierzonej wersji."));
    auto* fDane = new QFormLayout(gDane);
    obraz_ = new WyborObrazu;
    maska_ = new WyborMaski;
    fDane->addRow(QStringLiteral("&Obraz:"), obraz_);
    fDane->addRow(QStringLiteral("&Maska:"), maska_);
    lu->addWidget(gDane);

    auto* gParam = new QGroupBox(QStringLiteral("Parametry W4"));
    gParam->setToolTip(QStringLiteral("Harmonogram, fragment, sposób wyznaczania maksimum i przypisanie do rdzeni – stałe w całym przeglądzie."));
    auto* lp = new QVBoxLayout(gParam);
    auto* uwaga = new QLabel(QStringLiteral("Liczba wątków jest zmieniana automatycznie od 1 do maksimum."));
    uwaga->setObjectName("podpowiedz");
    uwaga->setWordWrap(true);
    uwaga->setToolTip(QStringLiteral("Pole „Wątki” jest pomijane – przegląd sam ustawia kolejne liczby wątków."));
    param_ = new ParametryWersji;
    param_->ustawWersje(Wersja::W4);
    lp->addWidget(uwaga);
    lp->addWidget(param_);
    lu->addWidget(gParam);

    auto* gPomiar = new QGroupBox(QStringLiteral("Przegląd"));
    gPomiar->setToolTip(QStringLiteral("Zakres liczby wątków i liczba powtórzeń dla każdej z nich. Mierzony jest sam algorytm."));
    auto* fp = new QFormLayout(gPomiar);
    maksWatkow_ = new QSpinBox;
    maksWatkow_->setRange(2, 64);
    maksWatkow_->setValue(std::max(2, static_cast<int>(cpuTopology().size())));
    maksWatkow_->setToolTip(QStringLiteral("Największa liczba wątków (domyślnie wszystkie wątki sprzętowe tego procesora: %1). "
                                           "Mierzone są wybrane wartości od 1 do maksimum, w tym liczba rdzeni fizycznych.").arg(topologia().logiczne));
    powtorzenia_ = new QSpinBox;
    powtorzenia_->setRange(1, 31);
    powtorzenia_->setValue(5);
    powtorzenia_->setToolTip(QStringLiteral("Powtórzenia dla każdej liczby wątków; wynik to mediana. Krótkie czasy wielowątkowe mają duży rozrzut – więcej powtórzeń daje gładszą krzywą."));
    fp->addRow(QStringLiteral("Maks. wątków:"), maksWatkow_);
    fp->addRow(QStringLiteral("&Powtórzenia:"), powtorzenia_);
    lu->addWidget(gPomiar);

    uruchom_ = new QPushButton(QStringLiteral("Zmierz skalowanie"));
    uruchom_->setObjectName("glowny");
    uruchom_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    uruchom_->setToolTip(QStringLiteral("Zmierz W4 dla kolejnych liczb wątków (Ctrl+R). Punkty pojawiają się na wykresie na bieżąco."));
    przerwij_ = new QPushButton(QStringLiteral("Przerwij"));
    przerwij_->setEnabled(false);
    przerwij_->setShortcut(Qt::Key_Escape);
    przerwij_->setToolTip(QStringLiteral("Przerwij pomiar po bieżącym powtórzeniu (Esc). Wyniki już zmierzonych wersji zostają."));
    auto* lb = new QHBoxLayout;
    lb->addWidget(uruchom_, 2);
    lb->addWidget(przerwij_, 1);
    lu->addLayout(lb);
    postep_ = new QProgressBar;
    postep_->setFormat(QStringLiteral("gotowe"));
    postep_->setValue(0);
    postep_->setToolTip(QStringLiteral("Postęp pomiaru: bieżąca wersja i powtórzenie. Na czas pomiaru inne zakładki nie mogą uruchomić własnego."));
    lu->addWidget(postep_);
    lu->addStretch();

    auto* przew = new QScrollArea;
    przew->setWidget(ustawienia);
    przew->setWidgetResizable(true);
    przew->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    przew->setFrameShape(QFrame::NoFrame);
    przew->setMinimumWidth(znaki(przew, 46));
    przew->setMaximumWidth(znaki(przew, 56));

    // --- wykres S(N)
    wykS_ = nowyWykres(QStringLiteral("Przyspieszenie S(N) = T(1) / T(N)"));
    sIdealne_ = new QLineSeries;
    sIdealne_->setName(QStringLiteral("idealne S = N"));
    sIdealne_->setPen(QPen(QColor("#9e9e9e"), 1.5, Qt::DashLine));
    sAmdahl_ = new QLineSeries;
    sAmdahl_->setName(QStringLiteral("prawo Amdahla"));
    sAmdahl_->setPen(QPen(QColor("#0072B2"), 1.5, Qt::DotLine));
    sZmierzone_ = new QLineSeries;
    sZmierzone_->setName(QStringLiteral("zmierzone (W4)"));
    sZmierzone_->setPen(QPen(kKolor, 2.2));
    sPunkty_ = new QScatterSeries;
    sPunkty_->setMarkerSize(8);
    sPunkty_->setColor(kKolor);
    sPunkty_->setBorderColor(Qt::white);
    for (QAbstractSeries* s : std::initializer_list<QAbstractSeries*>{ sIdealne_, sAmdahl_, sZmierzone_, sPunkty_ }) wykS_->addSeries(s);
    osSX_ = osWartosci(QStringLiteral("liczba wątków N"), "%.0f");
    osSY_ = osWartosci(QStringLiteral("przyspieszenie S(N)"), "%.0f");
    wykS_->addAxis(osSX_, Qt::AlignBottom);
    wykS_->addAxis(osSY_, Qt::AlignLeft);
    for (QAbstractSeries* s : wykS_->series()) { s->attachAxis(osSX_); s->attachAxis(osSY_); }
    for (QLegendMarker* m : wykS_->legend()->markers(sPunkty_)) m->setVisible(false);
    // linie odniesienia topologii: 6 = jeden blok rdzeni (CCD), 12 = rdzenie fizyczne, 24 = wątki SMT
    // linie odniesienia z topologii TEGO procesora: rdzenie jednego bloku L3 (gdy bloków jest kilka),
    // wszystkie rdzenie fizyczne, wszystkie wątki sprzętowe (SMT)
    const Topologia tp = topologia();
    QVector<int> linie;
    if (tp.bloki > 1) linie << tp.rdzeniWBloku;
    if (tp.fizyczne != tp.logiczne) linie << tp.fizyczne;
    linie << tp.logiczne;
    for (int n : linie) {
        auto* l = new QLineSeries;
        l->setPen(QPen(QColor(0, 0, 0, 70), 1, Qt::DotLine));
        l->append(n, 0);
        l->append(n, 1000);
        l->setName(QString());
        wykS_->addSeries(l);
        l->attachAxis(osSX_);
        l->attachAxis(osSY_);
        for (QLegendMarker* m : wykS_->legend()->markers(l)) m->setVisible(false);
    }

    // --- wykres E(N)
    wykE_ = nowyWykres(QStringLiteral("Efektywność E(N) = S(N) / N"));
    sE_ = new QLineSeries;
    sE_->setName(QStringLiteral("efektywność"));
    sE_->setPen(QPen(kKolor, 2.2));
    sEPunkty_ = new QScatterSeries;
    sEPunkty_->setMarkerSize(8);
    sEPunkty_->setColor(kKolor);
    sEPunkty_->setBorderColor(Qt::white);
    wykE_->addSeries(sE_);
    wykE_->addSeries(sEPunkty_);
    osEX_ = osWartosci(QStringLiteral("liczba wątków N"), "%.0f");
    osEY_ = osWartosci(QStringLiteral("efektywność E(N)"), "%.1f");
    osEY_->setRange(0, 1.1);
    wykE_->addAxis(osEX_, Qt::AlignBottom);
    wykE_->addAxis(osEY_, Qt::AlignLeft);
    for (QAbstractSeries* s : wykE_->series()) { s->attachAxis(osEX_); s->attachAxis(osEY_); }
    wykE_->legend()->setVisible(false);

    {
        QStringList l;
        if (tp.bloki > 1) l << QStringLiteral("%1 = rdzenie jednego bloku L3 (CCD)").arg(tp.rdzeniWBloku);
        if (tp.fizyczne != tp.logiczne) l << QStringLiteral("%1 = wszystkie rdzenie fizyczne").arg(tp.fizyczne);
        l << QStringLiteral("%1 = wszystkie wątki sprzętowe").arg(tp.logiczne);
        podsumowanie_ = new QLabel(QStringLiteral("Kliknij „Zmierz skalowanie”. Pionowe linie odniesienia: ") + l.join(QStringLiteral(", ")) + '.');
    }
    podsumowanie_->setObjectName("podpowiedz");
    podsumowanie_->setWordWrap(true);
    podsumowanie_->setToolTip(QStringLiteral("Wniosek z przeglądu: największe przyspieszenie, dopasowany udział części sekwencyjnej s i granica 1/s z prawa Amdahla."));

    tabela_ = new QTableWidget(0, 4);
    tabela_->setHorizontalHeaderLabels({ "N", "czas", "S(N)", "E(N)" });
    tabela_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    tabela_->verticalHeader()->hide();
    tabela_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tabela_->setAlternatingRowColors(true);
    tabela_->setMaximumWidth(znaki(tabela_, 44));
    tabela_->setToolTip(QStringLiteral("Zmierzone wartości dla każdej liczby wątków."));
    {
        const QStringList tipy = { QStringLiteral("Liczba wątków N"), QStringLiteral("Mediana czasu samego algorytmu"),
                                   QStringLiteral("Przyspieszenie S(N) = T(1) / T(N)"), QStringLiteral("Efektywność E(N) = S(N) / N – 1,0 oznacza idealne wykorzystanie wątków") };
        for (int i = 0; i < tipy.size(); ++i) tabela_->horizontalHeaderItem(i)->setToolTip(tipy[i]);
    }

    auto* wykresy = new QWidget;
    auto* lw = new QVBoxLayout(wykresy);
    lw->setContentsMargins(0, 0, 0, 0);
    lw->addWidget(new WidokWykresu(wykS_), 3);
    lw->addWidget(new WidokWykresu(wykE_), 2);
    lw->addWidget(podsumowanie_);
    auto* prawa = new QSplitter(Qt::Horizontal);
    prawa->addWidget(wykresy);
    prawa->addWidget(tabela_);
    prawa->setStretchFactor(0, 4);

    auto* glowny = new QHBoxLayout(this);
    glowny->addWidget(przew);
    glowny->addWidget(prawa, 1);

    // podpowiedź punktu: N, czas, S(N), E(N)
    auto podpowiedz = [this](const QPointF& p, bool nad) {
        const int n = static_cast<int>(std::lround(p.x()));
        if (!nad || !wyniki_.contains(n) || !wyniki_.contains(1)) { QToolTip::hideText(); return; }
        const double t = wyniki_[n].algorytm.mediana, S = wyniki_[1].algorytm.mediana / t;
        QToolTip::showText(QCursor::pos(), QStringLiteral("<b>N = %1 wątków</b><br>czas: %2 (rozrzut %3 – %4)<br>S(N) = %5×<br>E(N) = %6")
            .arg(n).arg(tekstCzasu(t), tekstCzasu(wyniki_[n].algorytm.min), tekstCzasu(wyniki_[n].algorytm.max),
                        liczba(S, 2), liczba(S / n, 2)));
    };
    connect(sPunkty_, &QScatterSeries::hovered, this, podpowiedz);
    connect(sEPunkty_, &QScatterSeries::hovered, this, podpowiedz);
    connect(sAmdahl_, &QLineSeries::hovered, this, [this](const QPointF& p, bool nad) {
        if (nad) QToolTip::showText(QCursor::pos(), QStringLiteral("%1<br>N = %2: S = %3×").arg(sAmdahl_->name(), liczba(p.x(), 1), liczba(p.y(), 2)));
        else QToolTip::hideText();
    });

    connect(uruchom_, &QPushButton::clicked, this, &StronaSkalowanie::uruchom);
    connect(przerwij_, &QPushButton::clicked, wyk_, &Wykonawca::przerwij);
    connect(wyk_, &Wykonawca::postep, this, [this](int p, const QString& o) {
        postep_->setValue(p);
        postep_->setFormat(QStringLiteral("%p% – ") + o);
    });
    connect(wyk_, &Wykonawca::wynik, this, [this](int i, const Pomiar& p) {
        if (p.ok && i < watki_.size()) { wyniki_[watki_[i]] = p; odswiez(); }
    });
    connect(wyk_, &Wykonawca::koniec, this, [this](bool przerwano) {
        postep_->setValue(przerwano ? 0 : 100);
        postep_->setFormat(przerwano ? QStringLiteral("przerwano") : QStringLiteral("gotowe"));
        odswiezBlokade();
    });
    connect(&Koordynator::instancja(), &Koordynator::zmiana, this, &StronaSkalowanie::odswiezBlokade);
    odswiez();
}

QVector<int> StronaSkalowanie::listaWatkow() const
{
    QVector<int> l;
    const Topologia tp = topologia();
    QSet<int> zbior = { 1, 2, 3, 4, 6, 8, 10, 12, 14, 16, 20, 24, 32, 48, 64, tp.rdzeniWBloku, tp.fizyczne, tp.logiczne };
    for (int n : zbior) if (n <= maksWatkow_->value()) l << n;
    l << maksWatkow_->value();
    std::sort(l.begin(), l.end());
    l.erase(std::unique(l.begin(), l.end()), l.end());
    return l;
}

void StronaSkalowanie::odswiezBlokade()
{
    uruchom_->setEnabled(!Koordynator::instancja().zajety());
    przerwij_->setEnabled(wyk_->trwa());
}

void StronaSkalowanie::zastosuj(const GotoweUstawienie& u)
{
    obraz_->ustaw(pelnaSciezka(u.obraz));
    maska_->ustaw(u.maska);
    param_->ustaw(u.cfg);
    powtorzenia_->setValue(u.powtorzenia);
    maksWatkow_->setValue(u.maksWatkow);
}

void StronaSkalowanie::uruchom()
{
    watki_ = listaWatkow();
    QVector<Zlecenie> lista;
    for (int n : watki_) {
        Zlecenie z;
        z.zrodlo = obraz_->zrodlo();
        z.wersja = Wersja::W4;
        z.maska = maska_->maska();
        z.cfg = param_->konfiguracja();
        z.cfg.threads = n;
        z.powtorzenia = powtorzenia_->value();
        z.zWczytaniemIZapisem = false;
        z.porownajZW1 = false;
        lista << z;
    }
    wyniki_.clear();
    odswiez();
    if (wyk_->start(lista, QStringLiteral("Skalowanie wątków"))) {
        uruchom_->setEnabled(false);
        przerwij_->setEnabled(true);
        emit komunikat(QStringLiteral("Przegląd %1 liczb wątków…").arg(watki_.size()), 0);
    }
}

void StronaSkalowanie::odswiez()
{
    const int nmax = std::max(2, maksWatkow_->value());
    sZmierzone_->clear();
    sPunkty_->clear();
    sE_->clear();
    sEPunkty_->clear();
    sIdealne_->clear();
    sAmdahl_->clear();
    tabela_->setRowCount(0);

    const bool jestT1 = wyniki_.contains(1);
    const double t1 = jestT1 ? wyniki_[1].algorytm.mediana : 0;
    QVector<QPointF> sn;
    double smax = 1;
    int nNaj = 1;
    for (auto it = wyniki_.cbegin(); it != wyniki_.cend(); ++it) {
        const int n = it.key();
        const double t = it.value().algorytm.mediana;
        const double S = jestT1 ? t1 / t : 0;
        if (jestT1) {
            sZmierzone_->append(n, S);
            sPunkty_->append(n, S);
            sE_->append(n, S / n);
            sEPunkty_->append(n, S / n);
            sn << QPointF(n, S);
            if (S > smax) { smax = S; nNaj = n; }
        }
        const int r = tabela_->rowCount();
        tabela_->insertRow(r);
        const QStringList kom = { QString::number(n), tekstCzasu(t), jestT1 ? liczba(S, 2) : "–", jestT1 ? liczba(S / n, 2) : "–" };
        for (int c = 0; c < kom.size(); ++c) {
            auto* item = new QTableWidgetItem(kom[c]);
            item->setToolTip(tabela_->horizontalHeaderItem(c)->toolTip() + QStringLiteral(": ") + kom[c]);
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            tabela_->setItem(r, c, item);
        }
    }
    const double ymax = std::max(4.0, smax * 1.45);
    sIdealne_->append(1, 1);
    sIdealne_->append(std::min<double>(nmax, ymax), std::min<double>(nmax, ymax));
    // podziałki co 2, 4 albo 8 wątków (okrągłe liczby na osi N)
    const int krok = nmax <= 16 ? 2 : nmax <= 32 ? 4 : 8;
    for (QValueAxis* os : { osSX_, osEX_ }) {
        os->setRange(0, ((nmax + krok - 1) / krok) * krok);
        os->setTickType(QValueAxis::TicksDynamic);
        os->setTickAnchor(0);
        os->setTickInterval(krok);
    }
    osSY_->setRange(0, ymax);
    osSY_->applyNiceNumbers();

    if (sn.size() >= 3) {
        const double s = dopasujAmdahla(sn);
        for (int i = 0; i <= 100; ++i) {
            const double n = 1 + (nmax - 1) * i / 100.0;
            sAmdahl_->append(n, amdahl(n, s));
        }
        sAmdahl_->setName(QStringLiteral("prawo Amdahla, s = %1%").arg(liczba(100 * s, 1)));
        QString t = QStringLiteral("T(1) = <b>%1</b>. Największe przyspieszenie <b>%2×</b> dla %3 wątków. "
                                   "Dopasowany udział części sekwencyjnej <b>s = %4%</b> – według prawa Amdahla "
                                   "przyspieszenie nie przekroczy 1/s ≈ %5× przy dowolnej liczbie wątków.")
                        .arg(tekstCzasu(t1), liczba(smax), QString::number(nNaj), liczba(100 * s, 1),
                             s > 0 ? liczba(1.0 / s) : QStringLiteral("∞"));
        const Topologia tp = topologia();
        if (tp.fizyczne != tp.logiczne && wyniki_.contains(tp.fizyczne) && wyniki_.contains(tp.logiczne))
            t += QStringLiteral(" Z %1 do %2 wątków (drugi wątek sprzętowy na rdzeniu, SMT): %3× → %4×.")
                     .arg(tp.fizyczne).arg(tp.logiczne)
                     .arg(liczba(t1 / wyniki_[tp.fizyczne].algorytm.mediana), liczba(t1 / wyniki_[tp.logiczne].algorytm.mediana));
        podsumowanie_->setText(t);
    }
}
