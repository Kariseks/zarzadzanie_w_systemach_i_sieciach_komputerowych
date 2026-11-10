/**
 * @file historia.cpp
 * @brief Implementacja zakładki „Historia pomiarów” (patrz historia.h).
 */
#include "historia.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include "masks.h"
#include "topology.h"

namespace {
const char* nazwyHarm[] = { "static", "cyclic", "dynamic", "guided" };
const char* nazwyRed[] = { "lokalne", "atomic", "mutex", "osobne linie", "jedna linia" };
const char* nazwyPrzyp[] = { "system", "compact", "spread", "nosmt" };

/** Kolumny tabeli: nagłówek i podpowiedź. */
const QVector<QPair<QString, QString>>& kolumny()
{
    static const QVector<QPair<QString, QString>> k = {
        { "Godzina", "Chwila zakończenia pomiaru" },
        { "Seria", "Z której zakładki lub serii pochodzi pomiar" },
        { "Wersja", "Wersja programu W1–W5" },
        { "Obraz", "Obraz wejściowy" },
        { "Maska", "Maska (operator gradientowy)" },
        { "Parametry", "Parametry W4 (wątki, harmonogram, fragment, maksimum, rdzenie) albo W5 (grupa robocza, piksele na wątek, precyzja)" },
        { "Powt.", "Liczba mierzonych powtórzeń (po rozgrzewce)" },
        { "Algorytm", "Mediana czasu samego algorytmu" },
        { "min – maks", "Najkrótszy i najdłuższy czas algorytmu [ms]" },
        { "CV", "Współczynnik zmienności czasu algorytmu; powyżej 5 % pomiar jest niestabilny (zakłócenia)" },
        { "Wczytanie", "Mediana wczytania pliku (– gdy nie mierzono)" },
        { "Zapis", "Mediana zapisu JPG (– gdy nie mierzono)" },
        { "Cały program", "Mediana wczytanie + algorytm + zapis" },
        { "×W1", "Przyspieszenie samego algorytmu względem W1 (gdy W1 zmierzono dla tych danych)" },
        { "Zgodność", "Porównanie wyniku z W1 piksel po pikselu" },
        { "Warunki", "Zasilanie, profil, regulator częstotliwości i zajętość CPU przed serią" },
    };
    return k;
}
} // namespace

QString opisParametrow(const Zlecenie& z)
{
    const RunConfig& c = z.cfg;
    if (z.wersja == Wersja::W4)
        return QStringLiteral("%1 w., %2, %3 w., %4, %5")
            .arg(c.threads).arg(QString::fromLatin1(nazwyHarm[static_cast<int>(c.schedule)])).arg(c.chunkRows)
            .arg(QString::fromLatin1(nazwyRed[static_cast<int>(c.reduce)]), QString::fromLatin1(nazwyPrzyp[static_cast<int>(c.affinity)]))
            + (c.mech == ThreadMech::Spawn ? QStringLiteral(", nowe wątki") : QString());
    if (z.wersja == Wersja::W5)
        return QStringLiteral("%1×%2, %3 px/wątek, %4").arg(c.gpuWgX).arg(c.gpuWgY).arg(c.gpuPpw).arg(c.gpuFp32 ? "fp32" : "dokładna");
    return QStringLiteral("–");
}

StronaHistoria::StronaHistoria(QWidget* parent) : QWidget(parent)
{
    setWhatsThis(QStringLiteral("<b>Historia pomiarów</b><br>Każdy pomiar z każdej zakładki trafia tutaj razem z parametrami, "
                                "rozrzutem, współczynnikiem zmienności i warunkami pomiaru. Eksport do CSV pozwala porównać "
                                "serie testów bez przepisywania liczb."));
    tabela_ = new QTableWidget(0, kolumny().size());
    QStringList nag;
    for (const auto& k : kolumny()) nag << k.first;
    tabela_->setHorizontalHeaderLabels(nag);
    for (int i = 0; i < kolumny().size(); ++i) tabela_->horizontalHeaderItem(i)->setToolTip(kolumny()[i].second);
    tabela_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tabela_->horizontalHeader()->setStretchLastSection(true);
    tabela_->verticalHeader()->hide();
    tabela_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tabela_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tabela_->setAlternatingRowColors(true);
    tabela_->setSortingEnabled(false);
    tabela_->setToolTip(QStringLiteral("Wszystkie pomiary tej sesji, najnowszy na górze. Wiersze z ⚠ mają duży rozrzut lub złe warunki."));

    podsumowanie_ = new QLabel;
    podsumowanie_->setObjectName("podpowiedz");
    podsumowanie_->setToolTip(QStringLiteral("Liczba pomiarów w sesji i ile z nich jest niestabilnych albo wykonanych w złych warunkach."));
    auto* eksport = new QPushButton(QStringLiteral("Eksportuj do CSV…"));
    eksport->setToolTip(QStringLiteral("Zapisz całą historię do pliku CSV (separator przecinek, czasy w ms)."));
    auto* czysc = new QPushButton(QStringLiteral("Wyczyść"));
    czysc->setToolTip(QStringLiteral("Usuń wszystkie wpisy historii tej sesji."));

    auto* gora = new QHBoxLayout;
    gora->addWidget(podsumowanie_, 1);
    gora->addWidget(eksport);
    gora->addWidget(czysc);
    auto* l = new QVBoxLayout(this);
    l->addLayout(gora);
    l->addWidget(tabela_, 1);

    connect(eksport, &QPushButton::clicked, this, &StronaHistoria::eksportujCsv);
    connect(czysc, &QPushButton::clicked, this, [this] {
        if (pomiary_.isEmpty()) return;
        if (QMessageBox::question(this, QStringLiteral("Wyczyść historię"),
                                  QStringLiteral("Usunąć %1 pomiarów z historii tej sesji?").arg(pomiary_.size()))
            == QMessageBox::Yes) wyczysc();
    });
    odswiezPodsumowanie();
}

void StronaHistoria::dodaj(const Pomiar& p)
{
    pomiary_.prepend(p);
    tabela_->insertRow(0);
    const bool io = p.zlecenie.zWczytaniemIZapisem;
    const QStringList ostrz = p.warunki.ostrzezenia();
    const Mask* mk = findMask(p.zlecenie.maska.toStdString());
    const QStringList kom = {
        p.kiedy.toString(QStringLiteral("HH:mm:ss")),
        p.opisSerii,
        QStringLiteral("%1 %2").arg(nazwaWersji(p.zlecenie.wersja), opisWersji(p.zlecenie.wersja)),
        p.zlecenie.zrodlo.etykieta,
        mk ? QString::fromStdString(mk->name) : p.zlecenie.maska,
        opisParametrow(p.zlecenie),
        QString::number(p.zlecenie.powtorzenia),
        tekstCzasu(p.algorytm.mediana),
        QStringLiteral("%1 – %2").arg(liczba(p.algorytm.min), liczba(p.algorytm.max)),
        QStringLiteral("%1%2%").arg(p.algorytm.niestabilny() ? "⚠ " : "", liczba(100 * p.algorytm.cv, 1)),
        io ? tekstCzasu(p.wczytanie.mediana) : QStringLiteral("–"),
        io ? tekstCzasu(p.zapis.mediana) : QStringLiteral("–"),
        io ? tekstCzasu(p.calosc.mediana) : QStringLiteral("–"),
        p.czasW1Ms > 0 && p.zlecenie.wersja != Wersja::W1 ? QStringLiteral("%1×").arg(liczba(p.czasW1Ms / p.algorytm.mediana))
                                                           : QStringLiteral("–"),
        !p.porownano ? QStringLiteral("–") : p.identyczny ? QStringLiteral("✔ identyczny")
                                                          : QStringLiteral("różnica ≤ %1").arg(p.maksRoznica),
        (ostrz.isEmpty() ? QString() : QStringLiteral("⚠ ")) + p.warunki.opis(),
    };
    for (int c = 0; c < kom.size(); ++c) {
        auto* it = new QTableWidgetItem(kom[c]);
        const bool liczbowa = (c >= 6 && c <= 13);
        it->setTextAlignment(liczbowa ? (Qt::AlignRight | Qt::AlignVCenter) : (Qt::AlignLeft | Qt::AlignVCenter));
        QString tip = kolumny()[c].second + QStringLiteral(": ") + kom[c];
        if (c == 15 && !ostrz.isEmpty()) tip += QStringLiteral("<br>⚠ ") + ostrz.join(QStringLiteral("<br>⚠ "));
        it->setToolTip(tip);
        if (c == 2) {
            QPixmap px(12, 12);
            px.fill(kolorWersji(p.zlecenie.wersja));
            it->setIcon(QIcon(px));
        }
        if ((c == 9 && p.algorytm.niestabilny()) || (c == 15 && !ostrz.isEmpty())) it->setForeground(QColor("#b26a00"));
        tabela_->setItem(0, c, it);
    }
    odswiezPodsumowanie();
}

void StronaHistoria::wyczysc()
{
    pomiary_.clear();
    tabela_->setRowCount(0);
    odswiezPodsumowanie();
}

void StronaHistoria::odswiezPodsumowanie()
{
    int niest = 0, zle = 0;
    for (const Pomiar& p : pomiary_) {
        if (p.algorytm.niestabilny()) ++niest;
        if (!p.warunki.ostrzezenia().isEmpty()) ++zle;
    }
    if (pomiary_.isEmpty())
        podsumowanie_->setText(QStringLiteral("Brak pomiarów w tej sesji – każdy pomiar z dowolnej zakładki pojawi się tutaj."));
    else
        podsumowanie_->setText(QStringLiteral("Pomiarów: <b>%1</b> · niestabilnych (CV > 5 %): <b>%2</b> · w złych warunkach: <b>%3</b>")
                                   .arg(pomiary_.size()).arg(niest).arg(zle));
}

void StronaHistoria::eksportujCsv()
{
    if (pomiary_.isEmpty()) { emit komunikat(QStringLiteral("Historia jest pusta.")); return; }
    const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Eksportuj historię"),
                                                   QStringLiteral("historia_pomiarow.csv"), QStringLiteral("CSV (*.csv)"));
    if (f.isEmpty()) return;
    QFile plik(f);
    if (!plik.open(QIODevice::WriteOnly | QIODevice::Text)) { emit komunikat(QStringLiteral("Nie można zapisać %1").arg(f)); return; }
    QTextStream o(&plik);
    o << "czas,seria,wersja,obraz,szerokosc,wysokosc,maska,parametry,powtorzenia,algorytm_ms,algorytm_min_ms,algorytm_max_ms,"
         "cv,wczytanie_ms,zapis_ms,calosc_ms,w1_ms,identyczny_z_W1,maks_roznica,zasilacz,profil,regulator,epp,zajetosc_cpu\n";
    auto q = [](QString s) { s.replace('"', "\"\""); return '"' + s + '"'; };
    for (auto it = pomiary_.crbegin(); it != pomiary_.crend(); ++it) {   // od najstarszego
        const Pomiar& p = *it;
        const bool io = p.zlecenie.zWczytaniemIZapisem;
        o << p.kiedy.toString(Qt::ISODate) << ',' << q(p.opisSerii) << ',' << nazwaWersji(p.zlecenie.wersja) << ','
          << q(p.zlecenie.zrodlo.sciezka) << ',' << p.szer << ',' << p.wys << ',' << p.zlecenie.maska << ','
          << q(opisParametrow(p.zlecenie)) << ',' << p.zlecenie.powtorzenia << ',' << p.algorytm.mediana << ','
          << p.algorytm.min << ',' << p.algorytm.max << ',' << p.algorytm.cv << ','
          << (io ? QString::number(p.wczytanie.mediana) : QString()) << ',' << (io ? QString::number(p.zapis.mediana) : QString()) << ','
          << p.calosc.mediana << ',' << (p.czasW1Ms > 0 ? QString::number(p.czasW1Ms) : QString()) << ','
          << (p.porownano ? (p.identyczny ? "tak" : "nie") : "") << ',' << p.maksRoznica << ','
          << (p.warunki.zasilacz == 1 ? "zasilacz" : p.warunki.zasilacz == 0 ? "bateria" : "") << ','
          << p.warunki.profil << ',' << p.warunki.regulator << ',' << p.warunki.preferencja << ','
          << (p.warunki.zajetosc >= 0 ? QString::number(p.warunki.zajetosc, 'f', 1) : QString()) << '\n';
    }
    emit komunikat(QStringLiteral("Zapisano %1 pomiarów do %2").arg(pomiary_.size()).arg(f));
}
