/**
 * @file strona_przetwarzanie.cpp
 * @brief Implementacja zakładki „Przetwarzanie” (patrz strona_przetwarzanie.h).
 */
#include "strona_przetwarzanie.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>
#include <QPainter>
#include <QSet>
#include <QStyle>
#include <QLocale>

#include "masks.h"

#include "widgets.h"
#include "wykonawca.h"

namespace {
/** Mały kolorowy kwadrat do oznaczenia wersji (ten sam kolor co na wykresach). */
QIcon ikonaKoloru(const QColor& c)
{
    QPixmap px(14, 14);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(1, 1, 12, 12), 3, 3);
    return QIcon(px);
}
} // namespace

StronaPrzetwarzanie::StronaPrzetwarzanie(QWidget* parent) : QWidget(parent)
{
    setWhatsThis(QStringLiteral("<b>Przetwarzanie</b><br>Uruchamia jedną wersję programu na wybranym obrazie i pokazuje "
                                "obraz wejściowy, wykryte krawędzie oraz czas każdego etapu."));
    wyk_ = new Wykonawca(this);

    // ---------------- lewa kolumna: ustawienia ----------------
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

    auto* gWersja = new QGroupBox(QStringLiteral("Wersja programu"));
    gWersja->setToolTip(QStringLiteral("Wybierz wersję programu. Kolor wersji jest taki sam na wszystkich wykresach."));
    gWersja->setWhatsThis(QStringLiteral("<b>Wersja programu</b><br>Każda wersja W2–W5 to kod W1 z jednym rodzajem "
                                         "przyspieszenia. Najedź na wersję, żeby zobaczyć, co zmienia."));
    auto* lw = new QVBoxLayout(gWersja);
    wersje_ = new QButtonGroup(this);
    for (Wersja w : wszystkieWersje()) {
        auto* rb = new QRadioButton(QStringLiteral("%1  %2").arg(nazwaWersji(w), opisWersji(w)));
        rb->setIcon(ikonaKoloru(kolorWersji(w)));
        rb->setToolTip(pomocWersji(w));
        rb->setWhatsThis(pomocWersji(w));
        QString powod;
        if (!wersjaDostepna(w, &powod)) {
            rb->setEnabled(false);
            rb->setToolTip(pomocWersji(w) + QStringLiteral("<br><i>Niedostępna: %1</i>").arg(powod));
        }
        wersje_->addButton(rb, static_cast<int>(w));
        lw->addWidget(rb);
    }
    wersje_->button(static_cast<int>(Wersja::W4))->setChecked(true);
    lu->addWidget(gWersja);

    auto* gParam = new QGroupBox(QStringLiteral("Parametry wersji"));
    gParam->setToolTip(QStringLiteral("Ustawienia zależne od wersji: dla W4 wątki i harmonogram, dla W5 grupa robocza i precyzja. W1–W3 nie mają parametrów."));
    auto* lp = new QVBoxLayout(gParam);
    param_ = new ParametryWersji;
    param_->ustawWersje(Wersja::W4);
    lp->addWidget(param_);
    lu->addWidget(gParam);

    auto* gPomiar = new QGroupBox(QStringLiteral("Pomiar"));
    gPomiar->setToolTip(QStringLiteral("Jak mierzyć: liczba powtórzeń (wynik to mediana) i czy wliczać wczytanie i zapis pliku."));
    auto* fPomiar = new QFormLayout(gPomiar);
    powtorzenia_ = new QSpinBox;
    powtorzenia_->setRange(1, 51);
    powtorzenia_->setValue(5);
    powtorzenia_->setToolTip(QStringLiteral("Liczba mierzonych powtórzeń; wynik to mediana. Przed nimi jedno wywołanie "
                                            "rozgrzewające (przydział pamięci, ładowanie kodu) nie jest liczone."));
    zPlikiem_ = new QCheckBox(QStringLiteral("mierz wczytanie i zapis pliku"));
    zPlikiem_->setChecked(true);
    zPlikiem_->setToolTip(QStringLiteral("W każdym powtórzeniu obraz jest wczytywany z pliku, a wynik zapisywany do JPG – "
                                         "czas całego programu. Bez tego mierzony jest tylko sam algorytm."));
    fPomiar->addRow(QStringLiteral("&Powtórzenia:"), powtorzenia_);
    fPomiar->addRow(zPlikiem_);
    lu->addWidget(gPomiar);

    uruchom_ = new QPushButton(QStringLiteral("Uruchom"));
    uruchom_->setObjectName("glowny");
    uruchom_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    uruchom_->setToolTip(QStringLiteral("Uruchom pomiar (Ctrl+R)"));
    przerwij_ = new QPushButton(QStringLiteral("Przerwij"));
    przerwij_->setEnabled(false);
    przerwij_->setToolTip(QStringLiteral("Przerwij po bieżącym powtórzeniu (Esc)"));
    przerwij_->setShortcut(Qt::Key_Escape);
    auto* lb = new QHBoxLayout;
    lb->addWidget(uruchom_, 2);
    lb->addWidget(przerwij_, 1);
    lu->addLayout(lb);
    postep_ = new QProgressBar;
    postep_->setTextVisible(true);
    postep_->setValue(0);
    postep_->setFormat(QStringLiteral("gotowe"));
    postep_->setToolTip(QStringLiteral("Postęp pomiaru: bieżąca wersja i powtórzenie. Na czas pomiaru inne zakładki nie mogą uruchomić własnego."));
    lu->addWidget(postep_);
    lu->addStretch();

    auto* przewijanie = new QScrollArea;
    przewijanie->setWidget(ustawienia);
    przewijanie->setWidgetResizable(true);
    przewijanie->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    przewijanie->setFrameShape(QFrame::NoFrame);
    przewijanie->setMinimumWidth(znaki(przewijanie, 46));
    przewijanie->setMaximumWidth(znaki(przewijanie, 56));

    // ---------------- prawa część: obrazy i wyniki ----------------
    wejscie_ = new PodgladObrazu(QStringLiteral("Wejście"));
    wyjscie_ = new PodgladObrazu(QStringLiteral("Wynik – krawędzie"));
    PodgladObrazu::sprzez(wejscie_, wyjscie_);
    wejscie_->ustawOpisWartosci(QStringLiteral("jasność"));
    wyjscie_->ustawOpisWartosci(QStringLiteral("siła krawędzi (moduł gradientu)"));
    auto* obrazy = new QSplitter(Qt::Horizontal);
    wejscie_->setWhatsThis(QStringLiteral("<b>Obraz wejściowy</b> po zamianie na skalę szarości. Powiększenie i przesunięcie są wspólne z obrazem wyniku."));
    wyjscie_->setWhatsThis(QStringLiteral("<b>Wynik</b>: moduł gradientu przeskalowany do 0–255 – jasne piksele to krawędzie."));
    obrazy->addWidget(wejscie_);
    obrazy->addWidget(wyjscie_);

    auto* wyniki = new QWidget;
    wyniki->setObjectName("panelWynikow");
    auto* gw = new QGridLayout(wyniki);
    duzyCzas_ = new QLabel(QStringLiteral("–"));
    duzyCzas_->setObjectName("duzaLiczba");
    duzyCzas_->setToolTip(QStringLiteral("Mediana czasu samego algorytmu (od obrazu w pamięci do gotowego obrazu 0–255)."));
    przyspieszenie_ = new QLabel;
    przyspieszenie_->setObjectName("srednieLiczby");
    przyspieszenie_->setToolTip(QStringLiteral("Ile razy szybciej niż W1 na tym samym obrazie i masce (stosunek median czasu samego algorytmu)."));
    zgodnosc_ = new QLabel;
    zgodnosc_->setObjectName("odznaka");
    zgodnosc_->setToolTip(QStringLiteral("Wynik porównany piksel po pikselu z wynikiem W1. Wszystkie wersje powinny dać obraz identyczny."));
    szczegoly_ = new QLabel;
    szczegoly_->setObjectName("podpowiedz");
    szczegoly_->setWordWrap(true);
    szczegoly_->setTextFormat(Qt::RichText);
    szczegoly_->setToolTip(QStringLiteral("Szczegóły pomiaru: rozmiar obrazu, maska, liczba powtórzeń, rozrzut czasu (min–maks); dla W4 liczba wątków i nierównowaga obciążenia, dla W5 liczba wątków GPU."));
    auto* podpisCzasu = new QLabel(QStringLiteral("sam algorytm (mediana)"));
    podpisCzasu->setToolTip(QStringLiteral("Od obrazu w pamięci do gotowego obrazu 0–255, bez wczytania i zapisu pliku."));
    podpisCzasu->setObjectName("podpowiedz");
    pasekProgramu_ = new PasekCzasow;
    pasekProgramu_->setToolTip(QStringLiteral("Najedź na odcinek, żeby zobaczyć dokładny czas i udział."));
    pasekProgramu_->setWhatsThis(QStringLiteral("<b>Cały program</b><br>Wczytanie pliku, algorytm i zapis wyniku. "
                                                "Wczytanie i zapis są jednowątkowe – to część sekwencyjna, która według "
                                                "prawa Amdahla ogranicza przyspieszenie całego programu."));
    pasekAlgorytmu_ = new PasekCzasow;
    pasekAlgorytmu_->setToolTip(QStringLiteral("Najedź na odcinek, żeby zobaczyć dokładny czas i udział."));
    pasekAlgorytmu_->setWhatsThis(QStringLiteral("<b>Etapy algorytmu</b><br>Splot i moduł gradientu, skalowanie do 0–255 "
                                                 "i pozostałe (przydział buforów, synchronizacja). Dla W5: przesył na kartę, "
                                                 "obliczenia, przesył z karty i sterowanie z procesora."));
    pasekProgramu_->ustaw(QStringLiteral("Cały program"), {});
    pasekAlgorytmu_->ustaw(QStringLiteral("Etapy samego algorytmu"), {});

    auto* kol = new QVBoxLayout;
    kol->addWidget(duzyCzas_);
    kol->addWidget(podpisCzasu);
    kol->addWidget(przyspieszenie_);
    kol->addWidget(zgodnosc_);
    kol->addStretch();
    gw->addLayout(kol, 0, 0, 3, 1);
    gw->addWidget(pasekProgramu_, 0, 1);
    gw->addWidget(pasekAlgorytmu_, 1, 1);
    gw->addWidget(szczegoly_, 2, 1);
    gw->setColumnStretch(1, 1);
    gw->setColumnMinimumWidth(0, 170);

    wyniki->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* prawa = new QSplitter(Qt::Vertical);
    prawa->addWidget(obrazy);
    prawa->addWidget(wyniki);
    prawa->setStretchFactor(0, 1);
    prawa->setStretchFactor(1, 0);
    prawa->setChildrenCollapsible(false);

    auto* glowny = new QHBoxLayout(this);
    glowny->addWidget(przewijanie);
    glowny->addWidget(prawa, 1);

    // ---------------- połączenia ----------------
    connect(wersje_, &QButtonGroup::idClicked, this, [this](int id) { param_->ustawWersje(static_cast<Wersja>(id)); });
    connect(uruchom_, &QPushButton::clicked, this, &StronaPrzetwarzanie::uruchom);
    connect(przerwij_, &QPushButton::clicked, wyk_, &Wykonawca::przerwij);
    connect(wyk_, &Wykonawca::postep, this, [this](int proc, const QString& o) {
        postep_->setValue(proc);
        postep_->setFormat(QStringLiteral("%p% – ") + o);
    });
    connect(wyk_, &Wykonawca::wynik, this, [this](int, const Pomiar& p) { pokaz(p); });
    connect(wyk_, &Wykonawca::koniec, this, [this](bool przerwano) {
        postep_->setValue(przerwano ? 0 : 100);
        postep_->setFormat(przerwano ? QStringLiteral("przerwano") : QStringLiteral("gotowe"));
        przerwij_->setEnabled(false);
        odswiezBlokade();
    });
    connect(&Koordynator::instancja(), &Koordynator::zmiana, this, &StronaPrzetwarzanie::odswiezBlokade);
}

Wersja StronaPrzetwarzanie::wersja() const { return static_cast<Wersja>(wersje_->checkedId()); }

void StronaPrzetwarzanie::odswiezBlokade()
{
    const bool zajety = Koordynator::instancja().zajety();
    uruchom_->setEnabled(!zajety);
    przerwij_->setEnabled(wyk_->trwa());
}

void StronaPrzetwarzanie::zastosuj(const GotoweUstawienie& u)
{
    obraz_->ustaw(pelnaSciezka(u.obraz));
    maska_->ustaw(u.maska);
    if (auto* b = wersje_->button(static_cast<int>(u.wersja)); b && b->isEnabled()) b->setChecked(true);
    param_->ustawWersje(wersja());
    param_->ustaw(u.cfg);
    powtorzenia_->setValue(u.powtorzenia);
    zPlikiem_->setChecked(u.zWczytaniem);
}

void StronaPrzetwarzanie::otworzObraz(const QString& sciezka) { obraz_->dodajPlik(sciezka); }

bool StronaPrzetwarzanie::maWynik() const { return !ostatniWynik_.isNull(); }

void StronaPrzetwarzanie::zapiszWynik()
{
    if (ostatniWynik_.isNull()) return;
    const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Zapisz wynik"), QStringLiteral("krawedzie.png"),
                                                   QStringLiteral("Obraz PNG (*.png);;Obraz JPG (*.jpg)"));
    if (!f.isEmpty() && ostatniWynik_.save(f)) emit komunikat(QStringLiteral("Zapisano %1").arg(f));
}

void StronaPrzetwarzanie::uruchom()
{
    Zlecenie z;
    z.zrodlo = obraz_->zrodlo();
    z.wersja = wersja();
    z.maska = maska_->maska();
    z.cfg = param_->konfiguracja();
    z.powtorzenia = powtorzenia_->value();
    z.rozgrzewka = 1;
    z.zWczytaniemIZapisem = zPlikiem_->isChecked();
    z.porownajZW1 = true;

    // Czas bazowy W1 jest potrzebny do przyspieszenia: mierzony raz dla danego obrazu i maski.
    QVector<Zlecenie> lista;
    if (z.wersja != Wersja::W1) {
        Zlecenie b = z;
        b.wersja = Wersja::W1;
        b.powtorzenia = std::min(3, z.powtorzenia);
        b.zWczytaniemIZapisem = false;
        b.porownajZW1 = false;
        static QSet<QString> zmierzone;
        const QString k = z.zrodlo.sciezka + '|' + z.maska;
        if (!zmierzone.contains(k)) { lista << b; zmierzone.insert(k); }
    }
    lista << z;
    if (wyk_->start(lista, QStringLiteral("Przetwarzanie: %1").arg(nazwaWersji(z.wersja)))) {
        uruchom_->setEnabled(false);
        przerwij_->setEnabled(true);
        emit komunikat(QStringLiteral("Pomiar %1 na obrazie %2…").arg(nazwaWersji(z.wersja), z.zrodlo.etykieta), 0);
    }
}

void StronaPrzetwarzanie::pokaz(const Pomiar& p)
{
    if (!p.ok) {
        if (p.blad != QLatin1String("przerwano")) emit komunikat(QStringLiteral("Błąd: %1").arg(p.blad), 10000);
        return;
    }
    if (p.zlecenie.wersja != wersja() && p.zlecenie.wersja == Wersja::W1) return;   // pomiar bazowy – tylko do cache

    wejscie_->ustawObraz(p.wejscie);
    wyjscie_->ustawObraz(p.wynik);
    ostatniWynik_ = p.wynik;

    const QColor kw = kolorWersji(p.zlecenie.wersja);
    duzyCzas_->setText(tekstCzasu(p.algorytm.mediana));
    duzyCzas_->setStyleSheet(QStringLiteral("color: %1;").arg(kw.name()));

    if (p.zlecenie.wersja == Wersja::W1) {
        przyspieszenie_->setText(QStringLiteral("wersja bazowa"));
        przyspieszenie_->setToolTip(QStringLiteral("W1 jest punktem odniesienia – z nią porównywane są pozostałe wersje."));
    } else if (p.czasW1Ms > 0) {
        przyspieszenie_->setText(QStringLiteral("%1× szybciej niż W1").arg(liczba(p.czasW1Ms / p.algorytm.mediana)));
        przyspieszenie_->setToolTip(QStringLiteral("W1: %1 dla tego obrazu i maski").arg(tekstCzasu(p.czasW1Ms)));
    } else {
        przyspieszenie_->clear();
    }
    if (p.porownano) {
        const bool ok = p.identyczny || (p.zlecenie.wersja == Wersja::W5 && p.maksRoznica <= 1);
        zgodnosc_->setText(p.identyczny ? QStringLiteral("✔ identyczny z W1")
                                        : QStringLiteral("%1 różnica ≤ %2 względem W1").arg(ok ? "✔" : "✖").arg(p.maksRoznica));
        zgodnosc_->setProperty("stan", ok ? "ok" : "blad");
        zgodnosc_->style()->unpolish(zgodnosc_);
        zgodnosc_->style()->polish(zgodnosc_);
        zgodnosc_->setToolTip(QStringLiteral("Wynik porównany piksel po pikselu z wynikiem wersji W1."));
    }

    if (p.zlecenie.zWczytaniemIZapisem)
        pasekProgramu_->ustaw(QStringLiteral("Cały program: wczytanie, algorytm, zapis"), segmentyProgramu(p));
    else
        pasekProgramu_->ustaw(QStringLiteral("Cały program (nie mierzono – włącz „mierz wczytanie i zapis”)"), {});
    pasekAlgorytmu_->ustaw(QStringLiteral("Etapy samego algorytmu %1").arg(nazwaWersji(p.zlecenie.wersja)), segmentyAlgorytmu(p));

    QStringList s;
    s << QStringLiteral("%1 × %2 px, %3, %4 powtórzeń").arg(p.szer).arg(p.wys)
             .arg(QString::fromStdString(findMask(p.zlecenie.maska.toStdString())->name)).arg(p.zlecenie.powtorzenia);
    s << QStringLiteral("algorytm: mediana %1, min %2, maks %3, CV %4%")
             .arg(tekstCzasu(p.algorytm.mediana), tekstCzasu(p.algorytm.min), tekstCzasu(p.algorytm.max), liczba(100 * p.algorytm.cv, 1));
    if (p.algorytm.niestabilny())
        s << QStringLiteral("<span style='color:#b26a00'><b>⚠ pomiar niestabilny (CV > 5 %) – zamknij inne programy albo zwiększ liczbę powtórzeń</b></span>");
    for (const QString& o : p.warunki.ostrzezenia())
        s << QStringLiteral("<span style='color:#b26a00'>⚠ %1</span>").arg(o);
    if (p.zlecenie.wersja == Wersja::W4)
        s << QStringLiteral("wątki: %1, nierównowaga: %2").arg(p.watki).arg(tekstCzasu(p.nierownowagaMs));
    if (p.zlecenie.wersja == Wersja::W5 && p.watkiGpu > 0)
        s << QStringLiteral("wątki GPU (work-items): %1").arg(QLocale(QLocale::Polish).toString(p.watkiGpu));
    szczegoly_->setText(s.join(QStringLiteral("  ·  ")));
    emit komunikat(QStringLiteral("%1: %2 (sam algorytm)").arg(nazwaWersji(p.zlecenie.wersja), tekstCzasu(p.algorytm.mediana)));
}
