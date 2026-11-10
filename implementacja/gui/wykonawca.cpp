/**
 * @file wykonawca.cpp
 * @brief Implementacja klasy Wykonawca (patrz wykonawca.h).
 */
#include "wykonawca.h"

#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include "presets.h"

Wykonawca::Wykonawca(QObject* parent) : QObject(parent), przerwij_(std::make_shared<std::atomic<bool>>(false)) {}

Wykonawca::~Wykonawca()
{
    przerwij_->store(true);
    fut_.waitForFinished();
}

bool Wykonawca::start(const QVector<Zlecenie>& zlecenia, const QString& opis)
{
    if (trwa_ || Koordynator::instancja().zajety() || zlecenia.isEmpty()) return false;
    trwa_ = true;
    przerwij_->store(false);
    Koordynator::instancja().ustawZajety(true, opis);

    QPointer<Wykonawca> ja(this);
    auto flaga = przerwij_;
    fut_ = QtConcurrent::run([ja, flaga, zlecenia, opis] {
        const int n = zlecenia.size();
        bool przerwano = false;
        // Warunki odczytywane raz przed serią, poza mierzonym czasem (pomiar zajętości trwa ok. 0,2 s)
        const WarunkiPomiaru warunki = odczytajWarunki(true);
        for (int i = 0; i < n && !przerwano; ++i) {
            const Zlecenie& z = zlecenia[i];
            const QString kto = QStringLiteral("%1 (%2/%3)").arg(nazwaWersji(z.wersja)).arg(i + 1).arg(n);
            auto post = [ja, i, n, kto](int k, int wsz, const QString& o) {
                const int proc = static_cast<int>(100.0 * (i + static_cast<double>(k) / std::max(1, wsz)) / n);
                QMetaObject::invokeMethod(ja, [ja, proc, kto, o] { if (ja) emit ja->postep(proc, kto + " – " + o); },
                                          Qt::QueuedConnection);
            };
            Pomiar p = zmierz(z, *flaga, post);
            p.warunki = warunki;
            p.opisSerii = opis;
            if (flaga->load()) przerwano = true;
            QMetaObject::invokeMethod(ja, [ja, i, p] {
                if (!ja) return;
                emit ja->wynik(i, p);
                if (p.ok) Koordynator::instancja().zglosPomiar(p);
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(ja, [ja, przerwano] {
            if (!ja) return;
            ja->trwa_ = false;
            Koordynator::instancja().ustawZajety(false);
            emit ja->koniec(przerwano);
        }, Qt::QueuedConnection);
    });
    return true;
}
