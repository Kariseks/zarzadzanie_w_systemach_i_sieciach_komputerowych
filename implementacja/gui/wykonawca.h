/**
 * @file wykonawca.h
 * @brief Wykonywanie serii pomiarów w wątku roboczym bez blokowania interfejsu.
 */
#pragma once

#include <QFuture>
#include <QObject>
#include <QVector>

#include <atomic>
#include <memory>

#include "engine.h"

/**
 * @brief Uruchamia listę zleceń po kolei w osobnym wątku (QtConcurrent) i raportuje postęp.
 *
 * Sygnały są wysyłane w wątku interfejsu. Na czas pomiaru Koordynator jest oznaczony
 * jako zajęty, więc inne zakładki nie mogą uruchomić równoległego pomiaru.
 */
class Wykonawca : public QObject {
    Q_OBJECT
public:
    explicit Wykonawca(QObject* parent = nullptr);
    ~Wykonawca() override;

    /** @brief Rozpoczyna serię pomiarów; ignorowane, gdy trwa inny pomiar. @return czy wystartowano */
    bool start(const QVector<Zlecenie>& zlecenia, const QString& opis);
    /** @brief Prosi o przerwanie (po bieżącym powtórzeniu). */
    void przerwij() { przerwij_->store(true); }
    /** @brief Czy seria trwa. */
    bool trwa() const { return trwa_; }

signals:
    /** @brief Postęp całej serii w procentach i opis bieżącego kroku. */
    void postep(int procent, const QString& opis);
    /** @brief Wynik jednego zlecenia (indeks w liście). */
    void wynik(int indeks, const Pomiar& p);
    /** @brief Koniec serii (również po przerwaniu lub błędzie). */
    void koniec(bool przerwano);

private:
    std::shared_ptr<std::atomic<bool>> przerwij_;
    QFuture<void> fut_;
    bool trwa_ = false;
};
