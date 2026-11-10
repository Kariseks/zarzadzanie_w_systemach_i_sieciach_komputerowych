/**
 * @file strona_porownanie.cpp
 * @brief Implementacja zakładki „Porównanie wersji” (patrz strona_porownanie.h).
 */
#include "strona_porownanie.h"

#include <QApplication>
#include <QBarCategoryAxis>
#include <QBarSeries>
#include <QBarSet>
#include <QChart>
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHorizontalStackedBarSeries>
#include <QLabel>
#include <QLegendMarker>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedBarSeries>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QToolTip>
#include <QCursor>
#include <QValueAxis>
#include <QVBoxLayout>

#include "widgets.h"
#include "wykonawca.h"
#include "wykresy.h"

StronaPorownanie::StronaPorownanie(QWidget* parent) : QWidget(parent)
{
    setWhatsThis(QStringLiteral("<b>Porównanie wersji</b><br>Mierzy zaznaczone wersje po kolei na tym samym obrazie "
                                "i pokazuje czas samego algorytmu oraz czas całego programu z wczytaniem i zapisem."));
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

    auto* gWersje = new QGroupBox(QStringLiteral("Wersje do porównania"));
    gWersje->setToolTip(QStringLiteral("Zaznaczone wersje są mierzone po kolei. W1 jest zawsze mierzona, bo jest bazą przyspieszenia."));
    auto* lw = new QVBoxLayout(gWersje);
    for (Wersja w : wszystkieWersje()) {
        auto* cb = new QCheckBox(QStringLiteral("%1  %2").arg(nazwaWersji(w), opisWersji(w)));
        cb->setToolTip(pomocWersji(w));
        QString powod;
        const bool ok = wersjaDostepna(w, &powod);
        cb->setChecked(ok);
        cb->setEnabled(ok && w != Wersja::W1);
        if (w == Wersja::W1) cb->setToolTip(pomocWersji(w) + QStringLiteral("<br><i>Zawsze mierzona – to baza przyspieszenia.</i>"));
        if (!ok) cb->setToolTip(pomocWersji(w) + QStringLiteral("<br><i>Niedostępna: %1</i>").arg(powod));
        zaznacz_[w] = cb;
        lw->addWidget(cb);
    }
    lu->addWidget(gWersje);

    auto* gW4 = new QGroupBox(QStringLiteral("Parametry W4 (wątki)"));
    gW4->setToolTip(QStringLiteral("Ustawienia wersji wielowątkowej użyte w porównaniu (domyślnie: wszystkie wątki sprzętowe, dynamic, fragment 16)."));
    auto* l4 = new QVBoxLayout(gW4);
    paramW4_ = new ParametryWersji;
    paramW4_->ustawWersje(Wersja::W4);
    l4->addWidget(paramW4_);
    lu->addWidget(gW4);
    auto* gW5 = new QGroupBox(QStringLiteral("Parametry W5 (GPU)"));
    gW5->setToolTip(QStringLiteral("Ustawienia wersji na karcie graficznej użyte w porównaniu."));
    auto* l5 = new QVBoxLayout(gW5);
    paramW5_ = new ParametryWersji;
    paramW5_->ustawWersje(Wersja::W5);
    l5->addWidget(paramW5_);
    lu->addWidget(gW5);

    auto* gPomiar = new QGroupBox(QStringLiteral("Pomiar"));
    gPomiar->setToolTip(QStringLiteral("Liczba powtórzeń każdej wersji i czy mierzyć cały program z plikami."));
    auto* fp = new QFormLayout(gPomiar);
    powtorzenia_ = new QSpinBox;
    powtorzenia_->setRange(1, 51);
    powtorzenia_->setValue(5);
    powtorzenia_->setToolTip(QStringLiteral("Powtórzenia każdej wersji; wynik to mediana, słupki pokazują min–maks w tabeli."));
    zPlikiem_ = new QCheckBox(QStringLiteral("mierz wczytanie i zapis pliku"));
    zPlikiem_->setChecked(true);
    zPlikiem_->setToolTip(QStringLiteral("Potrzebne do dolnego wykresu (cały program)."));
    fp->addRow(QStringLiteral("&Powtórzenia:"), powtorzenia_);
    fp->addRow(zPlikiem_);
    lu->addWidget(gPomiar);

    uruchom_ = new QPushButton(QStringLiteral("Porównaj wersje"));
    uruchom_->setObjectName("glowny");
    uruchom_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    uruchom_->setToolTip(QStringLiteral("Zmierz zaznaczone wersje po kolei (Ctrl+R)"));
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

    // --- wykresy i tabela
    wykAlg_ = nowyWykres(QStringLiteral("Sam algorytm – czas i przyspieszenie względem W1"));
    widokAlg_ = new WidokWykresu(wykAlg_);
    widokAlg_->setToolTip(QStringLiteral("Mediana czasu samego algorytmu każdej wersji. Najedź na słupek – czas, rozrzut i przyspieszenie. Prawy przycisk – zapis do PNG."));
    widokAlg_->setWhatsThis(QStringLiteral("<b>Sam algorytm</b><br>Czas od obrazu w pamięci do gotowego obrazu 0–255. W legendzie: czas i przyspieszenie względem W1."));
    wykProg_ = nowyWykres(QStringLiteral("Cały program – wczytanie, algorytm, zapis"));
    widokProg_ = new WidokWykresu(wykProg_);
    widokProg_->setToolTip(QStringLiteral("Cały program: wczytanie, algorytm i zapis. Najedź na odcinek – czas etapu. Prawy przycisk – zapis do PNG."));
    widokProg_->setWhatsThis(QStringLiteral("<b>Cały program</b><br>Wczytanie i zapis nie zależą od wersji i są jednowątkowe. "
                                            "Im szybszy algorytm, tym większy ich udział – przyspieszenie całego programu "
                                            "ogranicza prawo Amdahla."));
    podsumowanie_ = new QLabel(QStringLiteral("Wybierz obraz i kliknij „Porównaj wersje”."));
    podsumowanie_->setObjectName("podpowiedz");
    podsumowanie_->setWordWrap(true);
    podsumowanie_->setToolTip(QStringLiteral("Wniosek z bieżącego porównania, liczony automatycznie z wyników."));

    tabela_ = new QTableWidget(0, 8);
    tabela_->setHorizontalHeaderLabels({ "Wersja", "Algorytm", "min – maks", "×W1", "Wczytanie", "Zapis", "Cały program", "×W1 całość" });
    {
        const QStringList tipy = {
            QStringLiteral("Wersja programu (kolor jak na wykresach)"),
            QStringLiteral("Mediana czasu samego algorytmu"),
            QStringLiteral("Najkrótszy i najdłuższy czas algorytmu z powtórzeń [ms] – rozrzut pomiaru"),
            QStringLiteral("Przyspieszenie samego algorytmu względem W1"),
            QStringLiteral("Wczytanie i dekodowanie pliku obrazu (jeden wątek)"),
            QStringLiteral("Kodowanie i zapis wyniku do JPG (jeden wątek)"),
            QStringLiteral("Wczytanie + algorytm + zapis w jednym powtórzeniu (mediana)"),
            QStringLiteral("Przyspieszenie całego programu względem W1 – ograniczone przez wczytanie i zapis (prawo Amdahla)"),
        };
        for (int i = 0; i < tipy.size(); ++i) tabela_->horizontalHeaderItem(i)->setToolTip(tipy[i]);
    }
    tabela_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tabela_->horizontalHeader()->setStretchLastSection(false);
    tabela_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tabela_->verticalHeader()->hide();
    tabela_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tabela_->setAlternatingRowColors(true);
    tabela_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tabela_->setToolTip(QStringLiteral("Mediany czasów; ×W1 – ile razy szybciej niż W1."));

    // Wykresy pod sobą (każdy na pełną szerokość) w podzakładce „Wykresy”, tabela w „Tabela” –
    // na ekranie laptopa (ok. 1280×800 logicznie) wykresy dostają całą wysokość. Podsumowanie zawsze widoczne.
    auto* wykresy = new QSplitter(Qt::Vertical);
    wykresy->addWidget(widokAlg_);
    wykresy->addWidget(widokProg_);
    wykresy->setChildrenCollapsible(false);
    auto* podzakladki = new QTabWidget;
    podzakladki->addTab(wykresy, QStringLiteral("Wykresy"));
    podzakladki->addTab(tabela_, QStringLiteral("Tabela"));
    podzakladki->setTabToolTip(0, QStringLiteral("Czas samego algorytmu i całego programu dla każdej wersji"));
    podzakladki->setTabToolTip(1, QStringLiteral("Wszystkie liczby: mediany, rozrzut, przyspieszenia, wczytanie i zapis"));
    auto* prawa = new QWidget;
    auto* lp = new QVBoxLayout(prawa);
    lp->setContentsMargins(0, 0, 0, 0);
    lp->addWidget(podsumowanie_);
    lp->addWidget(podzakladki, 1);

    auto* glowny = new QHBoxLayout(this);
    glowny->addWidget(przew);
    glowny->addWidget(prawa, 1);

    connect(uruchom_, &QPushButton::clicked, this, &StronaPorownanie::uruchom);
    connect(przerwij_, &QPushButton::clicked, wyk_, &Wykonawca::przerwij);
    connect(wyk_, &Wykonawca::postep, this, [this](int p, const QString& o) {
        postep_->setValue(p);
        postep_->setFormat(QStringLiteral("%p% – ") + o);
    });
    connect(wyk_, &Wykonawca::wynik, this, [this](int, const Pomiar& p) {
        if (p.ok) { wyniki_[p.zlecenie.wersja] = p; odswiez(); }
        else if (p.blad != QLatin1String("przerwano")) emit komunikat(QStringLiteral("Błąd: %1").arg(p.blad), 10000);
    });
    connect(wyk_, &Wykonawca::koniec, this, [this](bool przerwano) {
        postep_->setValue(przerwano ? 0 : 100);
        postep_->setFormat(przerwano ? QStringLiteral("przerwano") : QStringLiteral("gotowe"));
        odswiezBlokade();
    });
    connect(&Koordynator::instancja(), &Koordynator::zmiana, this, &StronaPorownanie::odswiezBlokade);
    odswiez();
}

void StronaPorownanie::odswiezBlokade()
{
    uruchom_->setEnabled(!Koordynator::instancja().zajety());
    przerwij_->setEnabled(wyk_->trwa());
}

void StronaPorownanie::zastosuj(const GotoweUstawienie& u)
{
    obraz_->ustaw(pelnaSciezka(u.obraz));
    maska_->ustaw(u.maska);
    paramW4_->ustaw(u.cfg);
    powtorzenia_->setValue(u.powtorzenia);
    zPlikiem_->setChecked(u.zWczytaniem);
}

void StronaPorownanie::uruchom()
{
    QVector<Zlecenie> lista;
    for (Wersja w : wszystkieWersje()) {
        if (!zaznacz_[w]->isChecked()) continue;
        Zlecenie z;
        z.zrodlo = obraz_->zrodlo();
        z.wersja = w;
        z.maska = maska_->maska();
        z.cfg = w == Wersja::W5 ? paramW5_->konfiguracja() : paramW4_->konfiguracja();
        z.powtorzenia = powtorzenia_->value();
        z.zWczytaniemIZapisem = zPlikiem_->isChecked();
        z.porownajZW1 = w != Wersja::W1;
        lista << z;
    }
    wyniki_.clear();
    odswiez();
    if (wyk_->start(lista, QStringLiteral("Porównanie wersji"))) {
        uruchom_->setEnabled(false);
        przerwij_->setEnabled(true);
        emit komunikat(QStringLiteral("Porównanie %1 wersji na obrazie %2…").arg(lista.size()).arg(obraz_->zrodlo().etykieta), 0);
    }
}

void StronaPorownanie::odswiez()
{
    // ------- wykres 1: sam algorytm – jeden kolorowy słupek na wersję, czas i przyspieszenie w legendzie
    wykAlg_->removeAllSeries();
    for (QAbstractAxis* a : wykAlg_->axes()) { wykAlg_->removeAxis(a); delete a; }
    const double w1 = wyniki_.contains(Wersja::W1) ? wyniki_[Wersja::W1].algorytm.mediana : 0;
    QVector<Wersja> obecne;
    for (Wersja w : wszystkieWersje()) if (wyniki_.contains(w)) obecne << w;
    auto* sAlg = new QBarSeries;
    double maks = 1;
    for (Wersja w : obecne) {
        const double t = wyniki_[w].algorytm.mediana;
        QString nazwa = QStringLiteral("%1 %2: %3").arg(nazwaWersji(w), opisWersji(w), tekstCzasu(t));
        if (w1 > 0 && w != Wersja::W1) nazwa += QStringLiteral(" (×%1)").arg(liczba(w1 / t));
        auto* set = new QBarSet(nazwa);
        set->setColor(kolorWersji(w));
        set->setBorderColor(Qt::white);
        *set << t;
        const Pomiar& pw = wyniki_[w];
        QString tip = QStringLiteral("<b>%1 %2</b><br>sam algorytm: %3 (mediana z %4)<br>rozrzut: %5 – %6")
                          .arg(nazwaWersji(w), opisWersji(w), tekstCzasu(t)).arg(pw.zlecenie.powtorzenia)
                          .arg(tekstCzasu(pw.algorytm.min), tekstCzasu(pw.algorytm.max));
        if (w1 > 0 && w != Wersja::W1) tip += QStringLiteral("<br>przyspieszenie względem W1: <b>%1×</b>").arg(liczba(w1 / t));
        if (pw.porownano) tip += pw.identyczny ? QStringLiteral("<br>wynik identyczny z W1") : QStringLiteral("<br>maks. różnica względem W1: %1").arg(pw.maksRoznica);
        connect(set, &QBarSet::hovered, this, [tip](bool nad, int) {
            if (nad) QToolTip::showText(QCursor::pos(), tip); else QToolTip::hideText();
        });
        maks = std::max(maks, t);
        sAlg->append(set);
    }
    sAlg->setBarWidth(0.9);
    wykAlg_->addSeries(sAlg);
    auto* osK = new QBarCategoryAxis;
    osK->append(QStringLiteral(" "));
    osK->setVisible(false);
    auto* osY = osWartosci(QStringLiteral("czas [ms]"), maks < 10 ? "%.1f" : "%.0f");
    osY->setRange(0, maks * 1.08);
    osY->applyNiceNumbers();
    wykAlg_->addAxis(osK, Qt::AlignBottom);
    wykAlg_->addAxis(osY, Qt::AlignLeft);
    sAlg->attachAxis(osK);
    sAlg->attachAxis(osY);
    wykAlg_->legend()->setVisible(true);
    wykAlg_->legend()->setAlignment(Qt::AlignRight);

    // ------- wykres 2: cały program (wczytanie / algorytm / zapis)
    wykProg_->removeAllSeries();
    for (QAbstractAxis* a : wykProg_->axes()) { wykProg_->removeAxis(a); delete a; }
    auto* sProg = new QHorizontalStackedBarSeries;
    auto* sW = new QBarSet(QStringLiteral("wczytanie"));
    auto* sA = new QBarSet(QStringLiteral("algorytm"));
    auto* sZ = new QBarSet(QStringLiteral("zapis JPG"));
    sW->setColor(QColor("#9e9e9e"));
    sA->setColor(QColor("#37474f"));
    sZ->setColor(QColor("#757575"));
    sZ->setBrush(QBrush(QColor("#757575"), Qt::BDiagPattern));
    for (QBarSet* s : { sW, sA, sZ }) s->setBorderColor(Qt::white);
    QStringList katP;
    const double c1 = wyniki_.contains(Wersja::W1) ? wyniki_[Wersja::W1].calosc.mediana : 0;
    double maksP = 1;
    for (int i = obecne.size() - 1; i >= 0; --i) {   // W1 na górze
        const Pomiar& p = wyniki_[obecne[i]];
        *sW << p.wczytanie.mediana;
        *sA << p.algorytm.mediana;
        *sZ << p.zapis.mediana;
        maksP = std::max(maksP, p.calosc.mediana);
        QString e = QStringLiteral("%1  %2").arg(nazwaWersji(obecne[i]), tekstCzasu(p.calosc.mediana));
        if (c1 > 0 && obecne[i] != Wersja::W1) e += QStringLiteral(" (×%1)").arg(liczba(c1 / p.calosc.mediana));
        katP << e;
    }
    // podpowiedź odcinka: wersja, etap, czas i udział w całym programie
    const QVector<Wersja> odGory(obecne.rbegin(), obecne.rend());
    const QStringList etapy = { QStringLiteral("wczytanie pliku"), QStringLiteral("sam algorytm"), QStringLiteral("zapis JPG") };
    int nrEtapu = 0;
    for (QBarSet* s : { sW, sA, sZ }) {
        const QString etap = etapy[nrEtapu++];
        connect(s, &QBarSet::hovered, this, [this, s, etap, odGory](bool nad, int i) {
            if (!nad || i < 0 || i >= odGory.size()) { QToolTip::hideText(); return; }
            const Pomiar& p = wyniki_[odGory[i]];
            QToolTip::showText(QCursor::pos(), QStringLiteral("<b>%1 – %2</b><br>%3 (%4% całego programu: %5)")
                .arg(nazwaWersji(odGory[i]), etap, tekstCzasu(s->at(i)), liczba(100.0 * s->at(i) / p.calosc.mediana, 0),
                     tekstCzasu(p.calosc.mediana)));
        });
    }
    sProg->append(sW);
    sProg->append(sA);
    sProg->append(sZ);
    sProg->setBarWidth(0.6);
    wykProg_->addSeries(sProg);
    auto* osKP = new QBarCategoryAxis;
    osKP->append(katP.isEmpty() ? QStringList{ QStringLiteral(" ") } : katP);
    osKP->setLabelsFont(QApplication::font());
    auto* osX = osWartosci(QStringLiteral("czas [ms]"), "%.0f");
    osX->setRange(0, maksP * 1.05);
    osX->applyNiceNumbers();
    osKP->setTruncateLabels(false);
    wykProg_->addAxis(osKP, Qt::AlignLeft);
    wykProg_->addAxis(osX, Qt::AlignBottom);
    sProg->attachAxis(osKP);
    sProg->attachAxis(osX);
    wykProg_->legend()->setVisible(true);
    const bool zIo = !obecne.isEmpty() && wyniki_[obecne.first()].zlecenie.zWczytaniemIZapisem;
    wykProg_->setTitle(obecne.isEmpty() || zIo ? QStringLiteral("Cały program – wczytanie, algorytm, zapis")
                                               : QStringLiteral("Cały program – nie mierzono wczytania i zapisu (włącz w ustawieniach pomiaru)"));

    // ------- tabela
    tabela_->setRowCount(obecne.size());
    for (int r = 0; r < obecne.size(); ++r) {
        const Pomiar& p = wyniki_[obecne[r]];
        const bool io = p.zlecenie.zWczytaniemIZapisem;
        const QStringList kom = {
            QStringLiteral("%1 %2").arg(nazwaWersji(obecne[r]), opisWersji(obecne[r])),
            tekstCzasu(p.algorytm.mediana),
            QStringLiteral("%1 – %2").arg(liczba(p.algorytm.min), liczba(p.algorytm.max)),
            w1 > 0 ? QStringLiteral("%1×").arg(liczba(w1 / p.algorytm.mediana)) : QStringLiteral("–"),
            io ? tekstCzasu(p.wczytanie.mediana) : QStringLiteral("–"),
            io ? tekstCzasu(p.zapis.mediana) : QStringLiteral("–"),
            io ? tekstCzasu(p.calosc.mediana) : QStringLiteral("–"),
            io && c1 > 0 ? QStringLiteral("%1×").arg(liczba(c1 / p.calosc.mediana)) : QStringLiteral("–"),
        };
        for (int c = 0; c < kom.size(); ++c) {
            auto* it = new QTableWidgetItem(kom[c]);
            it->setToolTip(tabela_->horizontalHeaderItem(c)->toolTip() + QStringLiteral(": ") + kom[c]);
            it->setTextAlignment(c == 0 ? (Qt::AlignLeft | Qt::AlignVCenter) : (Qt::AlignRight | Qt::AlignVCenter));
            if (c == 0) {
                QPixmap px(12, 12);
                px.fill(kolorWersji(obecne[r]));
                it->setIcon(QIcon(px));
            }
            tabela_->setItem(r, c, it);
        }
    }

    // ------- podsumowanie słowne
    if (wyniki_.size() >= 2 && w1 > 0) {
        Wersja naj = Wersja::W1;
        for (Wersja w : obecne) if (wyniki_[w].algorytm.mediana < wyniki_[naj].algorytm.mediana) naj = w;
        QString t = QStringLiteral("Najszybsza: <b>%1 %2</b> – %3, czyli %4× szybciej niż W1 (%5).")
                        .arg(nazwaWersji(naj), opisWersji(naj), tekstCzasu(wyniki_[naj].algorytm.mediana),
                             liczba(w1 / wyniki_[naj].algorytm.mediana), tekstCzasu(w1));
        if (c1 > 0 && wyniki_[naj].zlecenie.zWczytaniemIZapisem) {
            const Pomiar& n = wyniki_[naj];
            t += QStringLiteral(" Cały program przyspieszył tylko %1×: wczytanie i zapis zajmują %2% jego czasu.")
                     .arg(liczba(c1 / n.calosc.mediana), liczba(100.0 * (n.wczytanie.mediana + n.zapis.mediana) / n.calosc.mediana, 0));
        }
        int niest = 0;
        for (Wersja w : obecne) if (wyniki_[w].algorytm.niestabilny()) ++niest;
        if (niest > 0) t += QStringLiteral(" <span style='color:#b26a00'>⚠ %1 z %2 pomiarów niestabilnych (CV > 5 %) – szczegóły w Historii pomiarów.</span>").arg(niest).arg(obecne.size());
        const QStringList ostrz = wyniki_[obecne.first()].warunki.ostrzezenia();
        if (!ostrz.isEmpty()) t += QStringLiteral(" <span style='color:#b26a00'>⚠ %1.</span>").arg(ostrz.join("; "));
        podsumowanie_->setText(t);
    } else if (wyniki_.isEmpty()) {
        podsumowanie_->setText(QStringLiteral("Wybierz obraz i kliknij „Porównaj wersje”. Wyniki pojawiają się po każdej wersji."));
    }
}

void StronaPorownanie::eksportujCsv()
{
    if (wyniki_.isEmpty()) return;
    const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Zapisz wyniki"), QStringLiteral("porownanie.csv"),
                                                   QStringLiteral("CSV (*.csv)"));
    if (f.isEmpty()) return;
    QFile plik(f);
    if (!plik.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream o(&plik);
    o << "wersja,obraz,maska,algorytm_ms,algorytm_min_ms,algorytm_max_ms,wczytanie_ms,zapis_ms,calosc_ms,identyczny_z_W1\n";
    for (auto it = wyniki_.cbegin(); it != wyniki_.cend(); ++it) {
        const Pomiar& p = it.value();
        o << nazwaWersji(it.key()) << ',' << p.zlecenie.zrodlo.sciezka << ',' << p.zlecenie.maska << ','
          << p.algorytm.mediana << ',' << p.algorytm.min << ',' << p.algorytm.max << ','
          << p.wczytanie.mediana << ',' << p.zapis.mediana << ',' << p.calosc.mediana << ','
          << (p.porownano ? (p.identyczny ? "tak" : "nie") : "baza") << '\n';
    }
    emit komunikat(QStringLiteral("Zapisano %1").arg(f));
}
